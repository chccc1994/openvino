// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <gtest/gtest.h>

#include <file_utils.h>
#include "openvino/util/shared_object.hpp"
#include <cpp/ie_plugin.hpp>

using namespace ::testing;
using namespace std;

class SharedObjectOVTests : public ::testing::Test {
protected:
    std::string get_mock_engine_name() {
        return FileUtils::makePluginLibraryName<char>(InferenceEngine::getIELibraryPath(),
                                                      std::string("mock_engine") + IE_BUILD_POSTFIX);
    }

    void loadDll(const string &libraryName) {
        shared_object = ov::util::load_shared_object(libraryName.c_str());
    }
    std::shared_ptr<void> shared_object;

    using CreateF = void(std::shared_ptr<InferenceEngine::IInferencePlugin>&);

    std::function<CreateF> make_std_function(const std::string& functionName) {
        std::function<CreateF> ptr(reinterpret_cast<CreateF*>(ov::util::get_symbol(shared_object, functionName.c_str())));
        return ptr;
    }
};

TEST_F(SharedObjectOVTests, canLoadExistedPlugin) {
    loadDll(get_mock_engine_name());
    EXPECT_NE(nullptr, shared_object.get());
}

TEST_F(SharedObjectOVTests, loaderThrowsIfNoPlugin) {
    EXPECT_THROW(loadDll("wrong_name"), std::runtime_error);
}

TEST_F(SharedObjectOVTests, canFindExistedMethod) {
    loadDll(get_mock_engine_name());

    auto factory = make_std_function("CreatePluginEngine");
    EXPECT_NE(nullptr, factory);
}

TEST_F(SharedObjectOVTests, throwIfMethodNofFoundInLibrary) {
    loadDll(get_mock_engine_name());
    EXPECT_THROW(make_std_function("wrong_function"), std::runtime_error);
}

TEST_F(SharedObjectOVTests, canCallExistedMethod) {
    loadDll(get_mock_engine_name());

    auto factory = make_std_function("CreatePluginEngine");
    std::shared_ptr<InferenceEngine::IInferencePlugin> ptr;
    EXPECT_NO_THROW(factory(ptr));
}
