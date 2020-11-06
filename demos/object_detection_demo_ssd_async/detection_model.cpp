/*
// Copyright (C) 2018-2019 Intel Corporation
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
#include "detection_model.h"
#include <samples/args_helper.hpp>
#include <samples/slog.hpp>

using namespace InferenceEngine;

DetectionModel::DetectionModel(std::string modelFileName, float confidenceThreshold, bool useAutoResize, const std::vector<std::string>& labels)
:ModelBase(modelFileName){
    this->useAutoResize = useAutoResize;
    this->confidenceThreshold = confidenceThreshold;
    this->labels = labels;
}

void DetectionModel::preprocess(const InputData& inputData, InferenceEngine::InferRequest::Ptr& request, MetaData*& metaData)
{
    auto imgData = inputData.asPtr<ImageInputData>();
    auto& img = imgData->inputImage;

    if (useAutoResize) {
        /* Just set input blob containing read image. Resize and layout conversionx will be done automatically */
        request->SetBlob(inputsNames[0], wrapMat2Blob(img));
    }
    else {
        /* Resize and copy data from the image to the input blob */
        Blob::Ptr frameBlob = request->GetBlob(inputsNames[0]);
        matU8ToBlob<uint8_t>(img, frameBlob);
    }

    metaData = new ImageMetaData(img);
}

cv::Mat DetectionModel::renderData(ResultBase* result)
{
    // Visualizing result data over source image
    cv::Mat outputImg = result->metaData.get()->asPtr<ImageMetaData>()->img.clone();

    for (auto obj : result->asPtr<DetectionResult>()->objects) {
        std::ostringstream conf;
        conf << ":" << std::fixed << std::setprecision(3) << obj.confidence;
        cv::putText(outputImg, obj.label + conf.str(),
            cv::Point2f(obj.x, obj.y - 5), cv::FONT_HERSHEY_COMPLEX_SMALL, 1,
            cv::Scalar(0, 0, 255));
        cv::rectangle(outputImg, obj, cv::Scalar(0, 0, 255));
    }
    if (result->metaData.get()->asPtr<ImageRetinaFaceMetaData>()) {
        for (auto face_landmarks : result->metaData.get()->asPtr<ImageRetinaFaceMetaData>()->landmarks_regression) {
            for (auto landmark : face_landmarks) {
                cv::circle(outputImg, landmark, 5, cv::Scalar(255, 0, 255), -1);
            }
        }
    }
    return outputImg;
}

std::vector<std::string> DetectionModel::loadLabels(const std::string & labelFilename){
    std::vector<std::string> labelsList;
    /** Read labels (if any)**/
    if (!labelFilename.empty()) {
        std::ifstream inputFile(labelFilename);
        std::string label;
        while (std::getline(inputFile, label)) {
            labelsList.push_back(label);
        }
        if (labelsList.empty())
            throw std::logic_error("File empty or not found: " + labelFilename);
    }
    return labelsList;
}
