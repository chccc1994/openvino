// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <map>
#include <string>
#include <memory>

#include "openvino/runtime/common.hpp"
#include "openvino/util/file_util.hpp"
#include "openvino/util/shared_object.hpp"

#include <ie_blob.h>
#include <file_utils.h>
#include <ie_preprocess.hpp>

#include <details/ie_so_pointer.hpp>

namespace InferenceEngine {

/**
 * @brief This class stores pre-process information for exact input
 */
class IPreProcessData : public std::enable_shared_from_this<IPreProcessData> {
public:
    /**
     * @brief Sets ROI blob to be resized and placed to the default input blob during pre-processing.
     * @param blob ROI blob.
     */
    virtual void setRoiBlob(const Blob::Ptr &blob) = 0;

    /**
     * @brief Gets pointer to the ROI blob used for a given input.
     * @return Blob pointer.
     */
    virtual Blob::Ptr getRoiBlob() const = 0;

    /**
     * @brief Executes input pre-processing with a given pre-processing information.
     * @param outBlob pre-processed output blob to be used for inference.
     * @param info pre-processing info that specifies resize algorithm and color format.
     * @param serial disable OpenMP threading if the value set to true.
     * @param batchSize batch size for pre-processing.
     */
    virtual void execute(Blob::Ptr &preprocessedBlob, const PreProcessInfo& info, bool serial, int batchSize = -1) = 0;

    virtual void isApplicable(const Blob::Ptr &src, const Blob::Ptr &dst) = 0;

protected:
    ~IPreProcessData() = default;
};

OPENVINO_PLUGIN_API void CreatePreProcessData(std::shared_ptr<IPreProcessData>& data);

#define OV_PREPROC_PLUGIN_CALL_STATEMENT(...)                                                      \
    if (!_ptr)                                                                                     \
        IE_THROW() << "Wrapper used in the OV_PREPROC_PLUGIN_CALL_STATEMENT was not initialized."; \
    try {                                                                                          \
        __VA_ARGS__;                                                                               \
    } catch (...) {                                                                                \
        ::InferenceEngine::details::Rethrow();                                                     \
    }

class PreProcessDataPlugin {
    std::shared_ptr<void> _so = nullptr;
    std::shared_ptr<IPreProcessData> _ptr = nullptr;

public:
    PreProcessDataPlugin() {
#ifdef OPENVINO_STATIC_LIBRARY
        CreatePreProcessData(_ptr);
        if (!_ptr)
            IE_THROW() << "Failed to create IPreProcessData for G-API based preprocessing";
#else
        ov::util::FilePath libraryName = ov::util::to_file_path(std::string("inference_engine_preproc") + std::string(IE_BUILD_POSTFIX));
        ov::util::FilePath preprocLibraryPath = FileUtils::makePluginLibraryName(getInferenceEngineLibraryPath(), libraryName);

        if (!FileUtils::fileExist(preprocLibraryPath)) {
            IE_THROW() << "Please, make sure that pre-processing library "
                << ov::util::from_file_path(::FileUtils::makePluginLibraryName({}, libraryName)) << " is in "
                << getIELibraryPath();
        }

        using CreateF = void(std::shared_ptr<IPreProcessData>& data);
        _so = ov::util::load_shared_object(preprocLibraryPath.c_str());
        reinterpret_cast<CreateF *>(ov::util::get_symbol(_so, "CreatePreProcessData"))(_ptr);
#endif
    }

    void setRoiBlob(const Blob::Ptr &blob) {
        OV_PREPROC_PLUGIN_CALL_STATEMENT(_ptr->setRoiBlob(blob));
    }

    Blob::Ptr getRoiBlob() const {
        OV_PREPROC_PLUGIN_CALL_STATEMENT(return _ptr->getRoiBlob());
    }

    void execute(Blob::Ptr &preprocessedBlob, const PreProcessInfo& info, bool serial, int batchSize = -1) {
        OV_PREPROC_PLUGIN_CALL_STATEMENT(_ptr->execute(preprocessedBlob, info, serial, batchSize));
    }

    void isApplicable(const Blob::Ptr &src, const Blob::Ptr &dst) {
        OV_PREPROC_PLUGIN_CALL_STATEMENT(return _ptr->isApplicable(src, dst));
    }
};

#undef OV_PREPROC_PLUGIN_CALL_STATEMENT

using PreProcessDataPtr = std::shared_ptr<PreProcessDataPlugin>;

inline PreProcessDataPtr CreatePreprocDataHelper() {
    return std::make_shared<PreProcessDataPlugin>();
}

}  // namespace InferenceEngine
