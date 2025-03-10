// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <node_context.hpp>

#include "default_opset.hpp"

namespace ngraph {
namespace frontend {
namespace pdpd {
namespace op {
NamedOutputs fill_any_like(const NodeContext& node) {
    const auto x = node.get_ng_input("X");
    auto dtype = node.get_attribute<ngraph::element::Type>("dtype", element::undefined);
    const auto value = node.get_attribute<float>("value");
    if (dtype == element::undefined) {
        // when type does not define, use the input type
        dtype = x.get_element_type();
    }
    const std::vector<element::Type> supported_type = {element::i32,
                                                       element::i64,
                                                       element::f16,
                                                       element::f32,
                                                       element::f64};
    const bool valid_type =
        std::any_of(supported_type.begin(), supported_type.end(), [dtype](const element::Type& type) {
            return dtype == type;
        });
    PDPD_ASSERT(valid_type, "fill_any_like only supports i32, i64, f16, f32, f64");
    const auto value_node = default_opset::Constant::create(dtype, {1}, {value});
    const auto shape_node = std::make_shared<default_opset::ShapeOf>(x);

    return node.default_single_output_mapping({std::make_shared<default_opset::Broadcast>(value_node, shape_node)},
                                              {"Out"});
}

}  // namespace op
}  // namespace pdpd
}  // namespace frontend
}  // namespace ngraph
