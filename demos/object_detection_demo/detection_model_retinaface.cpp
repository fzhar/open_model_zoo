/*
// Copyright (C) 2018-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "detection_model_retinaface.h"
#include <samples/slog.hpp>
#include <ngraph/ngraph.hpp>

using namespace InferenceEngine;

ModelRetinaFace::ModelRetinaFace(const std::string& modelFileName, float confidenceThreshold,
    bool useAutoResize, bool shouldDetectMasks, const std::vector<std::string>& labels)
    :DetectionModel(modelFileName, confidenceThreshold, useAutoResize, labels), shouldDetectMasks(shouldDetectMasks) {
    anchorCfg.push_back({ 32, { 32,16 }, 16, { 1.0 } });
    anchorCfg.push_back({ 16, { 8,4 }, 16, { 1.0 } });
    anchorCfg.push_back({ 8, { 2,1 }, 16, { 1.0 } });

    generate_anchors_fpn();

    landmark_std = 0.2; //shouldDetectMasks ? 0.2 : 1.0;
}

void ModelRetinaFace::prepareInputsOutputs(InferenceEngine::CNNNetwork & cnnNetwork){
    // --------------------------- Configure input & output ---------------------------------------------
    // --------------------------- Prepare input blobs -----------------------------------------------------
    slog::info << "Checking that the inputs are as the demo expects" << slog::endl;
    InputsDataMap inputInfo(cnnNetwork.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("This demo accepts networks that have only one input");
    }
    InputInfo::Ptr& input = inputInfo.begin()->second;
    std::string imageInputName = inputInfo.begin()->first;
    inputsNames.push_back(imageInputName);
    input->setPrecision(Precision::U8);
    if (useAutoResize) {
        input->getPreProcess().setResizeAlgorithm(ResizeAlgorithm::RESIZE_BILINEAR);
        input->getInputData()->setLayout(Layout::NHWC);
    }
    else {
        input->getInputData()->setLayout(Layout::NCHW);
    }

    //--- Reading image input parameters
    imageInputName = inputInfo.begin()->first;
    const TensorDesc& inputDesc = inputInfo.begin()->second->getTensorDesc();
    netInputHeight = getTensorHeight(inputDesc);
    netInputWidth = getTensorWidth(inputDesc);

    // --------------------------- Prepare output blobs -----------------------------------------------------
    slog::info << "Checking that the outputs are as the demo expects" << slog::endl;

    InferenceEngine::OutputsDataMap outputInfo(cnnNetwork.getOutputsInfo());

    std::vector<int> outputsSizes[OT_MAX];

    for (auto& output : outputInfo) {
        output.second->setPrecision(InferenceEngine::Precision::FP32);
        output.second->setLayout(InferenceEngine::Layout::NCHW);
        outputsNames.push_back(output.first);

        EOutputType type= OT_MAX;
        if (output.first.find("bbox") != -1) {
            type = OT_BBOX;
        }
        else if (output.first.find("cls") != -1) {
            type = OT_SCORES;
        }
        else if(output.first.find("landmark") != -1) {
            type = OT_LANDMARK;
        }
        else if(shouldDetectMasks && output.first.find("type") != -1) {
            type = OT_MASKSCORES;
        }
        else {
            continue;
        }

        size_t num = output.second->getDims()[2];
        size_t i = 0;
        for (; i < outputsSizes[type].size(); i++)
        {
            if (num < outputsSizes[type][i])
            {
                break;
            }
        }
        separateOutputsNames[type].insert(separateOutputsNames[type].begin()+i,output.first);
        outputsSizes[type].insert(outputsSizes[type].begin() + i, num);
    }

    if (outputsNames.size()!=9 && outputsNames.size() != 12)
        throw std::logic_error("Expected 12 or 9 output blobs");
}

std::vector<ModelRetinaFace::Anchor> _ratio_enum(const ModelRetinaFace::Anchor& anchor, std::vector<double> ratios) {
    std::vector<ModelRetinaFace::Anchor> retVal;
    auto w = anchor.getWidth();
    auto h = anchor.getHeight();
    auto xCtr = anchor.getXCenter();
    auto yCtr = anchor.getYCenter();
    for (auto ratio : ratios)
    {
        auto size = w * h;
        auto size_ratio = size / ratio;
        auto ws = std::round(sqrt(size_ratio));
        auto hs = std::round(ws * ratio);
        retVal.push_back({ xCtr - 0.5 * (ws - 1), yCtr - 0.5 * (hs - 1), xCtr + 0.5 * (ws - 1), yCtr + 0.5 * (hs - 1) });
    }
    return retVal;
}

std::vector<ModelRetinaFace::Anchor> _scale_enum(const ModelRetinaFace::Anchor& anchor, std::vector<double> scales) {
    std::vector<ModelRetinaFace::Anchor> retVal;
    auto w = anchor.getWidth();
    auto h = anchor.getHeight();
    auto xCtr = anchor.getXCenter();
    auto yCtr = anchor.getYCenter();
    for (auto scale : scales)
    {
        auto ws = w * scale;
        auto hs = h * scale;
        retVal.push_back({ xCtr - 0.5 * (ws - 1), yCtr - 0.5 * (hs - 1), xCtr + 0.5 * (ws - 1), yCtr + 0.5 * (hs - 1) });
    }
    return retVal;
}

    
std::vector<ModelRetinaFace::Anchor> generate_anchors(int base_size, const std::vector<double>& ratios, const std::vector<double>& scales) {
    ModelRetinaFace::Anchor base_anchor{ 0, 0, (double)base_size - 1, (double)base_size - 1 };
    auto ratio_anchors = _ratio_enum(base_anchor, ratios);
    std::vector<ModelRetinaFace::Anchor> retVal;

    for (auto ra : ratio_anchors)
    {
        auto addon = _scale_enum(ra, scales);
        retVal.insert(retVal.end(), addon.begin(), addon.end());
    }
    return retVal;
}

void ModelRetinaFace::generate_anchors_fpn()
{
    auto cfg = anchorCfg;
    std::sort(cfg.begin(), cfg.end(), [](AnchorCfgLine& x, AnchorCfgLine& y) {return x.stride > y.stride; });

    std::vector<ModelRetinaFace::Anchor> anchors;
    for (auto cfgLine : cfg)
    {
        auto anchors = generate_anchors(cfgLine.baseSize, cfgLine.ratios, cfgLine.scales);
        _anchors_fpn.emplace(cfgLine.stride,anchors);
    }
}

std::vector<int> nms(const std::vector<ModelRetinaFace::Anchor>& boxes, const std::vector<double>& scores, double thresh) {
    std::vector<double> areas(boxes.size());
    for (int i = 0; i < boxes.size(); i++)
    {
        areas[i] = (boxes[i].right - boxes[i].left) * (boxes[i].bottom - boxes[i].top);
    }
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&scores](int o1, int o2) { return scores[o1] > scores[o2]; });

    int ordersNum = 0;
    for (; scores[order[ordersNum]] >= 0; ordersNum++);

    std::vector<int> keep;
    bool shouldContinue = true;
    for(int i=0;shouldContinue && i<ordersNum;i++)
    {
        auto idx1 = order[i];
        if (idx1 >= 0) // ?
        {
            keep.push_back(idx1);
            shouldContinue = false;

            for (int j = i + 1; j < ordersNum; j++)
            {
                //if (j == i) continue;
                auto idx2 = order[j];
                if (idx2 >= 0)
                {
                    shouldContinue = true;

                    double overlappingWidth = fmin(boxes[idx1].right, boxes[idx2].right) - fmax(boxes[idx1].left, boxes[idx2].left);
                    double overlappingHeight = fmin(boxes[idx1].bottom, boxes[idx2].bottom) - fmax(boxes[idx1].top, boxes[idx2].top);

                    auto intersection = overlappingWidth > 0 && overlappingHeight > 0 ? overlappingWidth * overlappingHeight : 0;

                    auto overlap = intersection / (areas[idx1] + areas[idx2] - intersection);
                    if (overlap >= thresh)
                    {
                        order[j]=-1;
                    }
                }
            }
        }
    }
    return keep;
}

std::vector<ModelRetinaFace::Anchor> _get_proposals(InferenceEngine::MemoryBlob::Ptr rawData, int anchor_num,
                                                    const std::vector<ModelRetinaFace::Anchor>& anchors) {
    auto desc = rawData->getTensorDesc();
    auto sz = desc.getDims();
    std::vector<ModelRetinaFace::Anchor> retVal(anchors.size());
    LockedMemory<const void> outputMapped = rawData->rmap();
    const float *memPtr = outputMapped.as<float*>();
    auto bbox_pred_len = sz[1] / anchor_num;
    auto blockWidth = sz[2] * sz[3];

    for (int i = 0; i < anchors.size(); i++) {
        auto offset = blockWidth * bbox_pred_len * (i % anchor_num) + (i / anchor_num);
        auto dx = memPtr[offset];
        auto dy = memPtr[offset + blockWidth];
        auto dw = memPtr[offset + blockWidth * 2];
        auto dh = memPtr[offset + blockWidth * 3];

        auto pred_ctr_x = dx * anchors[i].getWidth() + anchors[i].getXCenter();
        auto pred_ctr_y = dy * anchors[i].getHeight() + anchors[i].getYCenter();
        auto exp_dw = exp(dw);
        auto exp_dh = exp(dh);
        auto w = anchors[i].getWidth();
        auto h = anchors[i].getHeight();
        auto pred_w = exp_dw * w; //exp(dw) * anchors[i].getWidth();
        auto pred_h = exp_dh * h; //exp(dh) * anchors[i].getHeight();
        retVal[i] = ModelRetinaFace::Anchor({ pred_ctr_x - 0.5 * (pred_w - 1.0), pred_ctr_y - 0.5 * (pred_h - 1.0),
            pred_ctr_x + 0.5 * (pred_w - 1.0), pred_ctr_y + 0.5 * (pred_h - 1.0) });
    }
    return retVal;
}
std::vector<double> _get_scores(InferenceEngine::MemoryBlob::Ptr rawData, int anchor_num) {
    auto desc = rawData->getTensorDesc();
    auto sz = desc.getDims();

    size_t restAnchors = sz[1] - anchor_num;
    std::vector<double> retVal(restAnchors*sz[2] * sz[3]);

    LockedMemory<const void> outputMapped = rawData->rmap();
    const float *memPtr = outputMapped.as<float*>();

    for (size_t x = anchor_num; x < sz[1]; x++) {
        for (size_t y = 0; y < sz[2]; y++) {
            for (size_t z = 0; z < sz[3]; z++) {
                retVal[(y*sz[3] + z)*restAnchors + (x - anchor_num)] = memPtr[ (x*sz[2]+y)*sz[3]+z];
            }
        }
    }
    return retVal;
}

std::vector<double> _get_mask_scores(InferenceEngine::MemoryBlob::Ptr rawData, int anchor_num) {
    auto desc = rawData->getTensorDesc();
    auto sz = desc.getDims();

    size_t restAnchors = sz[1] - anchor_num*2;
    std::vector<double> retVal(restAnchors*sz[2] * sz[3]);

    LockedMemory<const void> outputMapped = rawData->rmap();
    const float *memPtr = outputMapped.as<float*>();

    for (size_t x = anchor_num*2; x < sz[1]; x++) {
        for (size_t y = 0; y < sz[2]; y++) {
            for (size_t z = 0; z < sz[3]; z++) {
                retVal[(y*sz[3] + z)*restAnchors + (x - anchor_num*2)] = memPtr[(x*sz[2] + y)*sz[3] + z];
            }
        }
    }

    return retVal;
}


std::vector<std::vector<cv::Point2f>> _get_landmarks(InferenceEngine::MemoryBlob::Ptr rawData, int anchor_num,
                                        const std::vector<ModelRetinaFace::Anchor>& anchors, double landmark_std) {
    auto desc = rawData->getTensorDesc();
    auto sz = desc.getDims();
    LockedMemory<const void> outputMapped = rawData->rmap();
    const float *memPtr = outputMapped.as<float*>();
    auto landmark_pred_len = sz[1] / anchor_num;
    auto stride = landmark_pred_len * sz[2] * sz[3];
    std::vector<std::vector<cv::Point2f>> retVal(anchors.size());

    for (int i = 0; i < anchors.size(); i++) {
        auto ctrX = anchors[i].getXCenter();
        auto ctrY = anchors[i].getYCenter();
        auto blockWidth = sz[2]*sz[3];
        retVal[i].reserve(ModelRetinaFace::LANDMARKS_NUM);
        for (int j = 0; j < ModelRetinaFace::LANDMARKS_NUM; j++) {
            auto deltaX = (i % 2 ? memPtr[stride + i / 2 + j * 2 * blockWidth] : memPtr[i / 2 + j * 2 * blockWidth])* landmark_std;
            auto deltaY = (i % 2 ? memPtr[stride + i / 2 + (j * 2 + 1)*blockWidth] : memPtr[i / 2 + (j * 2 + 1)*blockWidth]) *  landmark_std;

            retVal[i][j] = cv::Point2f( {static_cast<float>(deltaX * anchors[i].getWidth() + anchors[i].getXCenter()),
                                         static_cast<float>(deltaY * anchors[i].getHeight() + anchors[i].getYCenter())} );
        }
    }
    return retVal;
}

void ModelRetinaFace::preprocess(const InputData& inputData, InferenceEngine::InferRequest::Ptr& request, std::shared_ptr<MetaData>& metaData) {
    auto& img = inputData.asRef<ImageInputData>().inputImage;

    if (useAutoResize) {
        /* Just set input blob containing read image. Resize and layout conversionx will be done automatically */
        request->SetBlob(inputsNames[0], wrapMat2Blob(img));
    }
    else {
        /* Resize and copy data from the image to the input blob */
        Blob::Ptr frameBlob = request->GetBlob(inputsNames[0]);
        matU8ToBlob<uint8_t>(img, frameBlob);
    }

    metaData = std::make_shared<ImageRetinaFaceMetaData>(img);
}

std::unique_ptr<ResultBase>  ModelRetinaFace::postprocess(InferenceResult& infResult) {
    std::vector<Anchor> proposals_list;
    std::vector<double> scores_list;
    std::vector<std::vector<cv::Point2f>> landmarks_list;
    std::vector<double> mask_scores_list;

    for (int idx = 0; idx < anchorCfg.size(); idx++) {
        auto s = anchorCfg[idx].stride;
        auto anchors_fpn = _anchors_fpn[s];
        auto anchor_num = anchors_fpn.size();
        auto scores = _get_scores(infResult.outputsData[separateOutputsNames[OT_SCORES][idx]], anchor_num);
        auto bbox_deltas = infResult.outputsData[separateOutputsNames[OT_BBOX][idx]];
        auto sz = bbox_deltas->getTensorDesc().getDims();
        auto height = sz[2];
        auto width = sz[3];

        //--- Creating strided anchors plane
        std::vector<Anchor> anchors(height*width*anchor_num);
        for (int iw = 0; iw < width; iw++) {
            auto sw = iw * s;
            for (int ih = 0; ih < height; ih++) {
                auto sh = ih * s;
                for (int k = 0; k < anchor_num; k++) {
                    Anchor& anc = anchors[(ih*width + iw)*anchor_num + k];
                    anc.left = anchors_fpn[k].left + sw;
                    anc.top = anchors_fpn[k].top + sh;
                    anc.right = anchors_fpn[k].right + sw;
                    anc.bottom = anchors_fpn[k].bottom + sh;
                }
            }
        }

        auto proposals = _get_proposals(bbox_deltas, anchor_num, anchors);
        auto landmarks = _get_landmarks(infResult.outputsData[separateOutputsNames[OT_LANDMARK][idx]], anchor_num, anchors, landmark_std);
        std::vector<double> maskScores;

        if (shouldDetectMasks) {
            maskScores = _get_mask_scores(infResult.outputsData[separateOutputsNames[OT_MASKSCORES][idx]], anchor_num);
        }

        for (auto& sc : scores) {
            if (sc < confidenceThreshold) {
                sc = -1;
            }
        }

        if (scores.size()) {
            auto keep = nms(proposals, scores, 0.5);
            proposals_list.reserve(proposals_list.size() + keep.size());
            scores_list.reserve(scores_list.size() + keep.size());
            landmarks_list.reserve(landmarks_list.size() + keep.size());
            for (auto kp : keep) {
                proposals_list.push_back(proposals[kp]);
                landmarks_list.push_back(landmarks[kp]);
                scores_list.push_back(scores[kp]);
                if (shouldDetectMasks) {
                    mask_scores_list.push_back(maskScores[kp]);
                }
            }
        }
    }

    DetectionResult* result = new DetectionResult;
    *static_cast<ResultBase*>(result) = static_cast<ResultBase&>(infResult);
    auto sz = infResult.metaData->asRef<ImageRetinaFaceMetaData>().img.size();
    double scale_x = ((double)netInputWidth) / sz.width;
    double scale_y = ((double)netInputHeight) / sz.height;

    for (int i = 0; i < scores_list.size(); i++) {
        DetectedObject desc;
        desc.confidence = (float)scores_list[i];
        desc.x = (float)(proposals_list[i].left / scale_x);
        desc.y = (float)(proposals_list[i].top / scale_y);
        desc.width = (float) (proposals_list[i].getWidth() / scale_x);
        desc.height = (float) (proposals_list[i].getHeight() / scale_y);
        desc.labelID = 1;
        desc.label = "Face";

        if (desc.confidence > confidenceThreshold) {
            /** Filtering out objects with confidence < confidence_threshold probability **/
            result->objects.push_back(desc);
        }
    }

    /** scaling landmarks coordinates**/
    for (auto& face_landmarks : landmarks_list) {
        for (auto& landmark : face_landmarks) {
            landmark.x /= scale_x;
            landmark.y /= scale_y;
        }
    }

    infResult.metaData->asRef<ImageRetinaFaceMetaData>().landmarks_regression = std::move(landmarks_list);

    return std::unique_ptr<ResultBase>(result);;
}
