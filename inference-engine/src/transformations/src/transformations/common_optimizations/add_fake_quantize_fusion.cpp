// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/add_fake_quantize_fusion.hpp"
#include "transformations/utils/utils.hpp"

#include <memory>
#include <vector>

#include <ngraph/opsets/opset5.hpp>
#include <ngraph/rt_info.hpp>
#include <ngraph/pattern/op/wrap_type.hpp>
#include <ngraph/validation_util.hpp>
#include "itt.hpp"


NGRAPH_RTTI_DEFINITION(ngraph::pass::AddFakeQuantizeFusion, "AddFakeQuantizeFusion", 0);

ngraph::pass::AddFakeQuantizeFusion::AddFakeQuantizeFusion() {
    MATCHER_SCOPE(AddFakeQuantizeFusion);
    auto input_pattern = ngraph::pattern::any_input();
    auto const_pattern = ngraph::pattern::wrap_type<opset5::Constant>();
    auto add_pattern = ngraph::pattern::wrap_type<opset5::Add>({input_pattern, const_pattern},
                                                               pattern::consumers_count(1));
    auto fq_pattern = ngraph::pattern::wrap_type<opset5::FakeQuantize>({add_pattern,
                                                                        ngraph::pattern::any_input(),
                                                                        ngraph::pattern::any_input(),
                                                                        ngraph::pattern::any_input(),
                                                                        ngraph::pattern::any_input()});
    ngraph::matcher_pass_callback callback = [=](pattern::Matcher& m) {
        const auto& pattern_value_map = m.get_pattern_value_map();
        const auto& input = pattern_value_map.at(input_pattern);
        const auto& type = input.get_element_type();
        if (type.bitwidth() < element::f32.bitwidth())
            return false;
        auto fq = std::dynamic_pointer_cast<opset5::FakeQuantize>(pattern_value_map.at(fq_pattern).get_node_shared_ptr());
        if (!fq)
            return false;
        const auto& add_node = pattern_value_map.at(add_pattern).get_node_shared_ptr();
        auto add_const = std::dynamic_pointer_cast<opset5::Constant>(pattern_value_map.at(const_pattern).get_node_shared_ptr());
        if (!add_const)
            return false;
        std::shared_ptr<Node> new_const = add_const;
        auto const_shape = add_const->get_shape();
        size_t const_shape_size = shape_size(const_shape);
        bool is_single_value = const_shape_size == 1;

        if (!is_single_value) {
            float v;
            is_single_value = op::util::get_single_value(add_const, v);
            if (is_single_value) {
                new_const = std::make_shared<opset5::Constant>(add_const->get_element_type(), Shape{1}, v);
            }
        }

        if (!is_single_value) {
            // disallow constant shapes other than (N, 1, 1, ..., 1) or (1, C, 1, ..., 1)
            if (!(const_shape[0] > 1 && const_shape[0] == const_shape_size) &&
                !(const_shape.size() > 1 && const_shape[1] == const_shape_size)) {
                return false;
            }

            // Convolution+Add or MatMul+Add can be fused later
            // so don't fuse Add+FQ in that situation
            const auto& add_inputs = add_node->input_values();
            bool add_parent_is_conv_or_mm = std::any_of(add_inputs.begin(), add_inputs.end(),
                                                        [] (const Output<Node>& node) -> bool {
                                                            auto node_ptr = node.get_node();
                                                            return is_type<opset5::Convolution>(node_ptr) ||
                                                                   is_type<opset5::GroupConvolution>(node_ptr) ||
                                                                   is_type<opset5::ConvolutionBackpropData>(node_ptr) ||
                                                                   is_type<opset5::GroupConvolutionBackpropData>(node_ptr) ||
                                                                   is_type<opset5::MatMul>(node_ptr);
                                                        });
            if (add_parent_is_conv_or_mm)
                return false;
            auto fq_users = fq->get_users();
            // Concat LPT transformation supports per tensor quantization only
            bool fq_user_is_concat = std::any_of(fq_users.begin(), fq_users.end(),
                                                 [] (const Output<Node>& node) -> bool {
                                                     auto node_ptr = node.get_node();
                                                     return is_type<opset5::Concat>(node_ptr);
                                                 });
            if (fq_user_is_concat)
                return false;
            auto diff = fq->get_input_partial_shape(0).rank().get_length() - static_cast<Dimension::value_type>(const_shape.size());
            if (diff > 0) {
                // Reshape constants like (C, 1, 1) to (1, C, 1, 1)
                const_shape.insert(const_shape.begin(), diff, 1);
                new_const = std::make_shared<opset5::Reshape>(new_const,
                        op::Constant::create(element::u64, Shape{const_shape.size()}, const_shape), false);
            }
        }

        auto input_low_sub = std::make_shared<opset5::Subtract>(fq->input_value(1), new_const);
        std::shared_ptr<Node> new_input_low = get_constant_from_source(input_low_sub);
        if (!new_input_low)
            new_input_low = input_low_sub;
        auto input_high_sub = std::make_shared<opset5::Subtract>(fq->input_value(2), new_const);
        std::shared_ptr<Node> new_input_high = get_constant_from_source(input_high_sub);
        if (!new_input_high)
            new_input_high = input_high_sub;
        auto new_fq = register_new_node<opset5::FakeQuantize>(input,
                                                              new_input_low,
                                                              new_input_high,
                                                              fq->input_value(3),
                                                              fq->input_value(4),
                                                              fq->get_levels());
        new_fq->set_friendly_name(fq->get_friendly_name());
        copy_runtime_info({add_node, fq}, {new_input_low, new_input_high, new_fq});
        replace_node(fq, new_fq);
        return true;
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(fq_pattern, matcher_name);
    this->register_matcher(m, callback);
}
