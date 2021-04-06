    /*
// Copyright (C) 2021 Intel Corporation
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
#pragma once
#include "models/model_base.h"

#ifdef USE_VA
#include "vaapi_converter.h"
#endif

#ifdef USE_VA
#include <gpu/gpu_context_api_va.hpp>
using VAContextPtr = InferenceEngine::gpu::VAContext::Ptr;
#else
using VAContextPtr = void*;
#endif

class ImageModel : public ModelBase {
public:
    /// Constructor
    /// @param modelFileName name of model to load
    /// @param useAutoResize - if true, image is resized by IE.
    ImageModel(const std::string& modelFileName, bool useAutoResize);

    virtual InferenceEngine::ExecutableNetwork loadExecutableNetwork(const CnnConfig& cnnConfig, InferenceEngine::Core& core) override;

    virtual std::shared_ptr<InternalModelData> preprocess(const InputData& inputData, InferenceEngine::InferRequest::Ptr& request) override;

protected:
    bool useAutoResize;

    size_t netInputHeight = 0;
    size_t netInputWidth = 0;

    VAContextPtr sharedVAContext;

#ifdef USE_VA
    InferenceBackend::VaApiContext::Ptr va_context;

    std::unique_ptr<InferenceBackend::VaApiImagePool> resizedSurfacesPool;
    #endif
};
