# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import logging

import onnx.backend.test
from tests_compatibility import (
    BACKEND_NAME,
    skip_rng_tests,
    xfail_issue_33488,
    xfail_issue_33538,
    xfail_issue_33581,
    xfail_issue_33589,
    xfail_issue_33593,
    xfail_issue_33595,
    xfail_issue_33596,
    xfail_issue_33606,
    xfail_issue_33633,
    xfail_issue_33651,
    xfail_issue_38091,
    xfail_issue_38699,
    xfail_issue_38701,
    xfail_issue_38706,
    xfail_issue_38708,
    xfail_issue_38710,
    xfail_issue_38713,
    xfail_issue_38724,
    xfail_issue_38734,
    xfail_issue_38735,
    xfail_issue_39658,
    xfail_issue_39662,
    xfail_issue_44854,
    xfail_issue_44858,
    xfail_issue_44956,
    xfail_issue_44965,
    xfail_issue_44968,
    xfail_issue_45180,
    xfail_issue_47323,
    xfail_issue_47337,
    xfail_issue_48052,
    xfail_issue_49207,
    xfail_issue_49750,
    xfail_issue_52463,
    xfail_issue_55760,
    xfail_issue_58033,
    xfail_issue_63033,
    xfail_issue_63036,
    xfail_issue_63039,
    xfail_issue_63043,
    xfail_issue_63044,
    xfail_issue_63045,
    xfail_issue_63136,
    xfail_issue_63137,
    xfail_issue_63138,
    skip_issue_69443,
    xfail_issue_69444,
)
from tests_compatibility.test_onnx.utils.onnx_backend import OpenVinoTestBackend


def expect_fail(test_case_path, xfail):  # type: (str) -> None
    """Mark the test as expected to fail."""
    module_name, test_name = test_case_path.split(".")
    module = globals().get(module_name)
    if hasattr(module, test_name):
        xfail(getattr(module, test_name))
    else:
        logging.getLogger().warning(
            "Could not mark test as XFAIL, not found: %s", test_case_path
        )


OpenVinoTestBackend.backend_name = BACKEND_NAME

# This is a pytest magic variable to load extra plugins
# Uncomment the line below to enable the ONNX compatibility report
# pytest_plugins = "onnx.backend.test.report",

# import all test cases at global scope to make them visible to python.unittest
backend_test = onnx.backend.test.BackendTest(OpenVinoTestBackend, __name__)

skip_tests_general = [
    # Big model tests (see test_zoo_models.py):
    "test_bvlc_alexnet",
    "test_densenet121",
    "test_inception_v1",
    "test_inception_v2",
    "test_resnet50",
    "test_shufflenet",
    "test_squeezenet",
    "test_vgg19",
    "test_zfnet512",
]

for test in skip_tests_general:
    backend_test.exclude(test)

# NOTE: ALL backend_test.exclude CALLS MUST BE PERFORMED BEFORE THE CALL TO globals().update

OnnxBackendNodeModelTest = None
OnnxBackendSimpleModelTest = None
OnnxBackendPyTorchOperatorModelTest = None
OnnxBackendPyTorchConvertedModelTest = None
globals().update(backend_test.enable_report().test_cases)

tests_expected_to_fail = [
    (
        xfail_issue_49207,
        "OnnxBackendNodeModelTest.test_rnn_seq_length_cpu",
        "OnnxBackendNodeModelTest.test_simple_rnn_defaults_cpu",
        "OnnxBackendNodeModelTest.test_simple_rnn_with_initial_bias_cpu",
        "OnnxBackendNodeModelTest.test_gru_defaults_cpu",
        "OnnxBackendNodeModelTest.test_gru_seq_length_cpu",
        "OnnxBackendNodeModelTest.test_gru_with_initial_bias_cpu",
        "OnnxBackendNodeModelTest.test_lstm_defaults_cpu",
        "OnnxBackendNodeModelTest.test_lstm_with_initial_bias_cpu",
        "OnnxBackendNodeModelTest.test_lstm_with_peepholes_cpu",
    ),
    (
        skip_issue_69443,
        "OnnxBackendNodeModelTest.test_constant_pad_cpu",
        "OnnxBackendNodeModelTest.test_edge_pad_cpu",
        "OnnxBackendNodeModelTest.test_reflect_pad_cpu",
    ),
    (
        xfail_issue_39658,
        "OnnxBackendNodeModelTest.test_tile_cpu",
        "OnnxBackendNodeModelTest.test_tile_precomputed_cpu",
    ),
    (
        xfail_issue_39662,
        "OnnxBackendNodeModelTest.test_scatter_elements_with_negative_indices_cpu",
        "OnnxBackendNodeModelTest.test_gather_negative_indices_cpu",
    ),
    (xfail_issue_33633, "OnnxBackendNodeModelTest.test_maxpool_2d_dilations_cpu"),
    (
        xfail_issue_55760,
        "OnnxBackendNodeModelTest.test_argmax_negative_axis_keepdims_example_select_last_index_cpu",
        "OnnxBackendNodeModelTest.test_argmin_negative_axis_keepdims_example_select_last_index_cpu",
        "OnnxBackendNodeModelTest.test_argmax_negative_axis_keepdims_random_select_last_index_cpu",
        "OnnxBackendNodeModelTest.test_argmin_negative_axis_keepdims_random_select_last_index_cpu",
    ),
    (
        xfail_issue_38091,
        "OnnxBackendNodeModelTest.test_gather_negative_indices_cpu",
        "OnnxBackendNodeModelTest.test_dynamicquantizelinear_cpu",
        "OnnxBackendNodeModelTest.test_dynamicquantizelinear_expanded_cpu",
    ),
    (
        xfail_issue_52463,
        "OnnxBackendPyTorchOperatorModelTest.test_operator_add_size1_singleton_broadcast_cpu",
    ),
    (
        xfail_issue_47323,
        "OnnxBackendPyTorchOperatorModelTest.test_operator_add_broadcast_cpu",
        "OnnxBackendPyTorchOperatorModelTest.test_operator_addconstant_cpu",
        "OnnxBackendPyTorchOperatorModelTest.test_operator_add_size1_right_broadcast_cpu",
    ),
    (
        xfail_issue_38699,
        "OnnxBackendSimpleModelTest.test_gradient_of_add_and_mul_cpu",
        "OnnxBackendSimpleModelTest.test_gradient_of_add_cpu",
    ),
    (
        xfail_issue_33596,
        "OnnxBackendSimpleModelTest.test_sequence_model5_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model7_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model1_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model3_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model6_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model8_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model4_cpu",
        "OnnxBackendSimpleModelTest.test_sequence_model2_cpu",
        "OnnxBackendNodeModelTest.test_identity_sequence_cpu",
        "OnnxBackendNodeModelTest.test_if_seq_cpu",
    ),
    (
        xfail_issue_38701,
        "OnnxBackendSimpleModelTest.test_strnorm_model_monday_casesensintive_nochangecase_cpu",
        "OnnxBackendSimpleModelTest.test_strnorm_model_nostopwords_nochangecase_cpu",
        "OnnxBackendSimpleModelTest.test_strnorm_model_monday_empty_output_cpu",
        "OnnxBackendSimpleModelTest.test_strnorm_model_monday_insensintive_upper_twodim_cpu",
        "OnnxBackendSimpleModelTest.test_strnorm_model_monday_casesensintive_lower_cpu",
        "OnnxBackendSimpleModelTest.test_strnorm_model_monday_casesensintive_upper_cpu",
        "OnnxBackendNodeModelTest.test_strnormalizer_nostopwords_nochangecase_cpu",
        "OnnxBackendNodeModelTest.test_strnormalizer_export_monday_casesensintive_nochangecase_cpu",
        "OnnxBackendNodeModelTest.test_strnormalizer_export_monday_insensintive_upper_twodim_cpu",
        "OnnxBackendNodeModelTest.test_strnormalizer_export_monday_casesensintive_lower_cpu",
        "OnnxBackendNodeModelTest.test_strnormalizer_export_monday_empty_output_cpu",
        "OnnxBackendNodeModelTest.test_strnormalizer_export_monday_casesensintive_upper_cpu",
        "OnnxBackendNodeModelTest.test_cast_STRING_to_FLOAT_cpu",
        "OnnxBackendNodeModelTest.test_cast_FLOAT_to_STRING_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT_to_STRING_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT_to_STRING_expanded_cpu",
        "OnnxBackendNodeModelTest.test_castlike_STRING_to_FLOAT_cpu",
        "OnnxBackendNodeModelTest.test_castlike_STRING_to_FLOAT_expanded_cpu",
    ),
    (
        xfail_issue_33595,
        "OnnxBackendNodeModelTest.test_unique_not_sorted_without_axis_cpu",
        "OnnxBackendNodeModelTest.test_unique_sorted_with_negative_axis_cpu",
        "OnnxBackendNodeModelTest.test_unique_sorted_with_axis_cpu",
        "OnnxBackendNodeModelTest.test_unique_sorted_with_axis_3d_cpu",
        "OnnxBackendNodeModelTest.test_unique_sorted_without_axis_cpu",
    ),
    (
        xfail_issue_33651,
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_batch_onlybigrams_skip5_cpu",
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_onlybigrams_levelempty_cpu",
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_batch_onlybigrams_skip0_cpu",
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_uniandbigrams_skip5_cpu",
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_only_bigrams_skip0_cpu",
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_batch_uniandbigrams_skip5_cpu",
        "OnnxBackendNodeModelTest.test_tfidfvectorizer_tf_onlybigrams_skip5_cpu",
    ),
    (xfail_issue_38706, "OnnxBackendNodeModelTest.test_split_zero_size_splits_cpu"),
    (
        xfail_issue_38708,
        "OnnxBackendNodeModelTest.test_slice_default_steps_cpu",
        "OnnxBackendNodeModelTest.test_slice_negative_axes_cpu",
        "OnnxBackendNodeModelTest.test_slice_neg_steps_cpu",
        "OnnxBackendNodeModelTest.test_slice_neg_cpu",
        "OnnxBackendNodeModelTest.test_slice_cpu",
        "OnnxBackendNodeModelTest.test_slice_end_out_of_bounds_cpu",
        "OnnxBackendNodeModelTest.test_slice_start_out_of_bounds_cpu",
    ),
    (
        xfail_issue_33538,
        "OnnxBackendNodeModelTest.test_scan_sum_cpu",
        "OnnxBackendNodeModelTest.test_scan9_sum_cpu",
    ),
    (
        xfail_issue_49750,
        "OnnxBackendNodeModelTest.test_resize_downsample_scales_cubic_align_corners_cpu",
        "OnnxBackendNodeModelTest.test_resize_downsample_scales_cubic_A_n0p5_exclude_outside_cpu",
        "OnnxBackendNodeModelTest.test_resize_upsample_scales_cubic_A_n0p5_exclude_outside_cpu",
    ),
    (
        xfail_issue_33581,
        "OnnxBackendNodeModelTest.test_gather_elements_negative_indices_cpu",
    ),
    (
        xfail_issue_38713,
        "OnnxBackendNodeModelTest.test_momentum_cpu",
        "OnnxBackendNodeModelTest.test_nesterov_momentum_cpu",
        "OnnxBackendNodeModelTest.test_momentum_multiple_cpu",
    ),
    (
        xfail_issue_47337,
        "OnnxBackendNodeModelTest.test_onehot_without_axis_cpu",
        "OnnxBackendNodeModelTest.test_onehot_with_negative_axis_cpu",
        "OnnxBackendNodeModelTest.test_onehot_with_axis_cpu",
        "OnnxBackendNodeModelTest.test_onehot_negative_indices_cpu",
    ),
    (
        xfail_issue_33488,
        "OnnxBackendNodeModelTest.test_maxunpool_export_with_output_shape_cpu",
        "OnnxBackendNodeModelTest.test_maxunpool_export_without_output_shape_cpu",
    ),
    (
        xfail_issue_33589,
        "OnnxBackendNodeModelTest.test_isnan_cpu",
        "OnnxBackendNodeModelTest.test_isinf_positive_cpu",
        "OnnxBackendNodeModelTest.test_isinf_negative_cpu",
        "OnnxBackendNodeModelTest.test_isinf_cpu",
    ),
    (xfail_issue_38724, "OnnxBackendNodeModelTest.test_resize_tf_crop_and_resize_cpu"),
    (
        xfail_issue_33606,
        "OnnxBackendNodeModelTest.test_det_2d_cpu",
        "OnnxBackendNodeModelTest.test_det_nd_cpu",
    ),
    (
        xfail_issue_38734,
        "OnnxBackendNodeModelTest.test_adam_multiple_cpu",
        "OnnxBackendNodeModelTest.test_adam_cpu",
    ),
    (
        xfail_issue_38735,
        "OnnxBackendNodeModelTest.test_adagrad_multiple_cpu",
        "OnnxBackendNodeModelTest.test_adagrad_cpu",
    ),
    (
        xfail_issue_48052,
        "OnnxBackendNodeModelTest.test_training_dropout_cpu",
        "OnnxBackendNodeModelTest.test_training_dropout_mask_cpu",
        "OnnxBackendNodeModelTest.test_training_dropout_default_cpu",
        "OnnxBackendNodeModelTest.test_training_dropout_zero_ratio_cpu",
        "OnnxBackendNodeModelTest.test_training_dropout_default_mask_cpu",
        "OnnxBackendNodeModelTest.test_training_dropout_zero_ratio_mask_cpu",
    ),
    (
        xfail_issue_45180,
        "OnnxBackendNodeModelTest.test_reduce_sum_do_not_keepdims_example_cpu",
        "OnnxBackendNodeModelTest.test_reduce_sum_do_not_keepdims_random_cpu",
    ),
    (
        xfail_issue_44854,
        "OnnxBackendNodeModelTest.test_split_variable_parts_1d_cpu",
        "OnnxBackendNodeModelTest.test_split_variable_parts_2d_cpu",
        "OnnxBackendNodeModelTest.test_split_variable_parts_default_axis_cpu",
    ),
    (
        xfail_issue_44858,
        "OnnxBackendNodeModelTest.test_unsqueeze_axis_0_cpu",
        "OnnxBackendNodeModelTest.test_unsqueeze_axis_1_cpu",
        "OnnxBackendNodeModelTest.test_unsqueeze_axis_2_cpu",
        "OnnxBackendNodeModelTest.test_unsqueeze_negative_axes_cpu",
        "OnnxBackendNodeModelTest.test_unsqueeze_three_axes_cpu",
        "OnnxBackendNodeModelTest.test_unsqueeze_two_axes_cpu",
        "OnnxBackendNodeModelTest.test_unsqueeze_unsorted_axes_cpu",
    ),
    (
        xfail_issue_44956,
        "OnnxBackendNodeModelTest.test_loop11_cpu",
        "OnnxBackendNodeModelTest.test_range_int32_type_negative_delta_expanded_cpu",
        "OnnxBackendNodeModelTest.test_range_float_type_positive_delta_expanded_cpu",
    ),
    (
        xfail_issue_44965,
        "OnnxBackendNodeModelTest.test_loop13_seq_cpu",
        "OnnxBackendNodeModelTest.test_sequence_insert_at_back_cpu",
        "OnnxBackendNodeModelTest.test_sequence_insert_at_front_cpu",
    ),
    (
        xfail_issue_44968,
        "OnnxBackendNodeModelTest.test_squeeze_cpu",
        "OnnxBackendNodeModelTest.test_squeeze_negative_axes_cpu",
    ),
    (
        xfail_issue_33593,
        "OnnxBackendNodeModelTest.test_maxpool_with_argmax_2d_precomputed_strides_cpu",
        "OnnxBackendNodeModelTest.test_maxpool_with_argmax_2d_precomputed_pads_cpu",
    ),
    (xfail_issue_58033, "OnnxBackendNodeModelTest.test_einsum_batch_diagonal_cpu"),
    (
        xfail_issue_63033,
        "OnnxBackendNodeModelTest.test_batchnorm_epsilon_training_mode_cpu",
        "OnnxBackendNodeModelTest.test_batchnorm_example_training_mode_cpu",
    ),
    (xfail_issue_63036, "OnnxBackendNodeModelTest.test_convtranspose_autopad_same_cpu"),
    (
        xfail_issue_63039,
        "OnnxBackendNodeModelTest.test_div_uint8_cpu",
        "OnnxBackendNodeModelTest.test_mul_uint8_cpu",
        "OnnxBackendNodeModelTest.test_sub_uint8_cpu",
    ),
    (
        xfail_issue_63043,
        "OnnxBackendNodeModelTest.test_gru_batchwise_cpu",
        "OnnxBackendNodeModelTest.test_lstm_batchwise_cpu",
        "OnnxBackendNodeModelTest.test_simple_rnn_batchwise_cpu",
    ),
    (
        xfail_issue_38710,
        "OnnxBackendNodeModelTest.test_reshape_allowzero_reordered_cpu",
    ),
    (
        xfail_issue_63044,
        "OnnxBackendNodeModelTest.test_tril_cpu",
        "OnnxBackendNodeModelTest.test_tril_neg_cpu",
        "OnnxBackendNodeModelTest.test_tril_one_row_neg_cpu",
        "OnnxBackendNodeModelTest.test_tril_out_neg_cpu",
        "OnnxBackendNodeModelTest.test_tril_out_pos_cpu",
        "OnnxBackendNodeModelTest.test_tril_pos_cpu",
        "OnnxBackendNodeModelTest.test_tril_square_cpu",
        "OnnxBackendNodeModelTest.test_tril_square_neg_cpu",
        "OnnxBackendNodeModelTest.test_tril_zero_cpu",
        "OnnxBackendNodeModelTest.test_triu_cpu",
        "OnnxBackendNodeModelTest.test_triu_neg_cpu",
        "OnnxBackendNodeModelTest.test_triu_one_row_cpu",
        "OnnxBackendNodeModelTest.test_triu_out_neg_out_cpu",
        "OnnxBackendNodeModelTest.test_triu_out_pos_cpu",
        "OnnxBackendNodeModelTest.test_triu_pos_cpu",
        "OnnxBackendNodeModelTest.test_triu_square_cpu",
        "OnnxBackendNodeModelTest.test_triu_square_neg_cpu",
        "OnnxBackendNodeModelTest.test_triu_zero_cpu",
    ),
    (
        xfail_issue_63045,
        "OnnxBackendPyTorchConvertedModelTest.test_MaxPool1d_stride_padding_dilation_cpu",
        "OnnxBackendPyTorchConvertedModelTest.test_MaxPool2d_stride_padding_dilation_cpu",
    ),
    (
        skip_rng_tests,
        "OnnxBackendNodeModelTest.test_bernoulli_cpu",
        "OnnxBackendNodeModelTest.test_bernoulli_double_cpu",
        "OnnxBackendNodeModelTest.test_bernoulli_double_expanded_cpu",
        "OnnxBackendNodeModelTest.test_bernoulli_expanded_cpu",
        "OnnxBackendNodeModelTest.test_bernoulli_seed_cpu",
        "OnnxBackendNodeModelTest.test_bernoulli_seed_expanded_cpu",
    ),
    (
        xfail_issue_63136,
        "OnnxBackendNodeModelTest.test_castlike_BFLOAT16_to_FLOAT_cpu",
        "OnnxBackendNodeModelTest.test_castlike_DOUBLE_to_FLOAT16_cpu",
        "OnnxBackendNodeModelTest.test_castlike_DOUBLE_to_FLOAT_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT16_to_DOUBLE_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT16_to_FLOAT_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT_to_BFLOAT16_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT_to_DOUBLE_cpu",
        "OnnxBackendNodeModelTest.test_castlike_FLOAT_to_FLOAT16_cpu",
    ),
    (
        xfail_issue_63137,
        "OnnxBackendNodeModelTest.test_optional_get_element_cpu",
        "OnnxBackendNodeModelTest.test_optional_get_element_sequence_cpu",
        "OnnxBackendNodeModelTest.test_optional_has_element_cpu",
        "OnnxBackendNodeModelTest.test_optional_has_element_empty_cpu",
    ),
    (
        xfail_issue_63138,
        "OnnxBackendNodeModelTest.test_shape_end_1_cpu",
        "OnnxBackendNodeModelTest.test_shape_end_negative_1_cpu",
        "OnnxBackendNodeModelTest.test_shape_start_1_cpu",
        "OnnxBackendNodeModelTest.test_shape_start_1_end_2_cpu",
        "OnnxBackendNodeModelTest.test_shape_start_1_end_negative_1_cpu",
        "OnnxBackendNodeModelTest.test_shape_start_negative_1_cpu",
    ),
    (xfail_issue_69444, "OnnxBackendNodeModelTest.test_resize_downsample_scales_linear_align_corners_cpu"),
]

for test_group in tests_expected_to_fail:
    for test_case in test_group[1:]:
        expect_fail("{}".format(test_case), test_group[0])
