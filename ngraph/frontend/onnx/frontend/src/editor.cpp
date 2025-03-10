// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "editor.hpp"

#include <onnx/onnx_pb.h>
#include <onnx/shape_inference/implementation.h>

#include <fstream>

#include "detail/subgraph_extraction.hpp"
#include "edge_mapper.hpp"
#include "ngraph/file_util.hpp"
#include "ngraph/log.hpp"
#include "onnx_common/parser.hpp"
#include "onnx_common/utils.hpp"
#include "utils/onnx_internal.hpp"

using namespace ngraph;
using namespace ngraph::onnx_editor;

NGRAPH_SUPPRESS_DEPRECATED_START

namespace {
using namespace ONNX_NAMESPACE;

ValueInfoProto* find_graph_input(GraphProto& graph, const std::string& name) {
    for (int i = 0; i < graph.input_size(); ++i) {
        auto* input_desc = graph.mutable_input(i);
        if (input_desc->has_name() && input_desc->name() == name) {
            return input_desc;
        }
    }

    return nullptr;
}

ValueInfoProto* find_graph_output(GraphProto& graph, const std::string& name) {
    for (int i = 0; i < graph.output_size(); ++i) {
        auto* output_desc = graph.mutable_output(i);
        if (output_desc->has_name() && output_desc->name() == name) {
            return output_desc;
        }
    }

    return nullptr;
}

TensorProto* find_graph_initializer(GraphProto& graph, const std::string& name) {
    for (int i = 0; i < graph.initializer_size(); ++i) {
        auto* initializer_desc = graph.mutable_initializer(i);
        if (initializer_desc->has_name() && initializer_desc->name() == name)
            return initializer_desc;
    }

    return nullptr;
}

ValueInfoProto* find_graph_value_info(GraphProto& graph, const std::string& name) {
    for (int i = 0; i < graph.value_info_size(); ++i) {
        auto value_info = graph.mutable_value_info(i);
        if (value_info->name() == name) {
            return value_info;
        }
    }
    return nullptr;
}

void modify_input_type(ValueInfoProto& onnx_input, const element::Type_t elem_type) {
    if (!onnx_input.has_type()) {
        throw ngraph_error("The input is malformed - it doesn't contain the 'type' field. Cannot change the "
                           "data type. Input name: " +
                           onnx_input.name());
    }

    auto* type_proto = onnx_input.mutable_type();
    if (!type_proto->has_tensor_type()) {
        throw ngraph_error("The input is malformed - it doesn't contain the 'tensor_type' field. Cannot "
                           "change the data type. Input name: " +
                           onnx_input.name());
    }

    auto* tensor_type = type_proto->mutable_tensor_type();

    if (onnx_common::is_supported_ng_type(elem_type)) {
        tensor_type->set_elem_type(onnx_common::ng_to_onnx_data_type(elem_type));
    } else {
        throw ngraph_error("The input type for input '" + onnx_input.name() + "' cannot be set to: " +
                           element::Type(elem_type).get_type_name() + ". This type is not allowed in ONNX.");
    }
}

void add_dim_to_onnx_shape(const Dimension& dim, ONNX_NAMESPACE::TensorShapeProto& onnx_shape) {
    auto* new_dim = onnx_shape.add_dim();
    if (dim.is_static()) {
        new_dim->set_dim_value(dim.get_length());
    } else {
        // nGraph Dimension is also considered dynamic if it represents a constrained range
        // of allowed values as well as if it's unconstrained at all. ONNX cannot represent
        // ranged dimensions so this might not be 100% accurate. The modified ONNX model will
        // always have a fully dynamic dimension in this case.
        new_dim->set_dim_param("__dynamic_dimension__");
    }
}

void modify_input_shape(ValueInfoProto& onnx_input, const PartialShape& new_shape) {
    if (!onnx_input.has_type()) {
        throw ngraph_error("The input is malformed - it doesn't contain the 'type' field. Cannot change the "
                           "input shape. Input name: " +
                           onnx_input.name());
    }

    auto* type_proto = onnx_input.mutable_type();
    if (!type_proto->has_tensor_type()) {
        throw ngraph_error("The input is malformed - it doesn't contain the 'tensor_type' field. Cannot "
                           "change the input shape. Input name: " +
                           onnx_input.name());
    }

    auto* tensor_type = type_proto->mutable_tensor_type();
    if (new_shape.rank().is_dynamic()) {
        tensor_type->clear_shape();
    } else {
        // make a copy intentionally, in case of an exception the original model is not modified
        auto new_onnx_shape = tensor_type->shape();
        new_onnx_shape.clear_dim();

        for (const auto& dim : static_cast<std::vector<Dimension>>(new_shape)) {
            add_dim_to_onnx_shape(dim, new_onnx_shape);
        }

        *(tensor_type->mutable_shape()) = std::move(new_onnx_shape);
    }
}

template <typename T>
std::string extract_name(const T& input_or_initializer) {
    return input_or_initializer.name();
};

void modify_initializer(TensorProto& initializer,
                        const std::string& name,
                        const std::shared_ptr<ngraph::op::Constant> values,
                        ValueInfoProto* input) {
    const auto elem_type = values->get_element_type();
    if (!onnx_common::is_supported_ng_type(elem_type)) {
        throw ngraph_error("Initializer '" + name + "' type cannot be set to: " +
                           element::Type(elem_type).get_type_name() + ". This type is not allowed in ONNX.");
    }

    initializer.Clear();

    initializer.set_name(name);
    initializer.set_data_type(onnx_common::ng_to_onnx_data_type(values->get_element_type()));

    for (const auto& dim : values->get_shape()) {
        initializer.add_dims(dim);
    }

    const auto data_size_in_bytes =
        shape_size(values->get_shape()) * onnx_common::get_onnx_data_size(initializer.data_type());
    initializer.set_raw_data(values->get_data_ptr(), data_size_in_bytes);

    // update input with type and shape of initializer
    if (input) {
        auto tensor_type = input->mutable_type()->mutable_tensor_type();
        TensorShapeProto shape;
        for (int i = 0; i < initializer.dims_size(); ++i) {
            shape.add_dim()->set_dim_value(initializer.dims(i));
        }
        *tensor_type->mutable_shape() = std::move(shape);
        tensor_type->set_elem_type(initializer.data_type());
    }
}
class InferShapesAutoRelease {
public:
    InferShapesAutoRelease(std::shared_ptr<ONNX_NAMESPACE::ModelProto> model_proto)
        : m_model_proto{model_proto},
          m_infer_shapes_was_run{false} {}
    void infer_shapes() {
        ONNX_NAMESPACE::shape_inference::InferShapes(*m_model_proto);
        m_infer_shapes_was_run = true;
    }
    ~InferShapesAutoRelease() {
        try {
            if (m_infer_shapes_was_run) {
                m_model_proto->mutable_graph()->clear_value_info();
            }
        } catch (...) {
        }
    }

private:
    std::shared_ptr<ONNX_NAMESPACE::ModelProto> m_model_proto;
    bool m_infer_shapes_was_run;
};
}  // namespace

/// \brief A helper class used to hold the ModelProto object as its field
struct onnx_editor::ONNXModelEditor::Impl {
    std::shared_ptr<ONNX_NAMESPACE::ModelProto> m_model_proto;
    EdgeMapper m_edge_mapper;
    bool m_is_mapper_updated = false;

    Impl() = delete;

    Impl(const std::string& model_path)
        : m_model_proto{std::make_shared<ONNX_NAMESPACE::ModelProto>(onnx_common::parse_from_file(model_path))} {}

    Impl(std::istream& model_stream)
        : m_model_proto{std::make_shared<ONNX_NAMESPACE::ModelProto>(onnx_common::parse_from_istream(model_stream))} {}

#if defined(OPENVINO_ENABLE_UNICODE_PATH_SUPPORT) && defined(_WIN32)
    Impl(const std::wstring& model_path)
        : m_model_proto{std::make_shared<ONNX_NAMESPACE::ModelProto>(onnx_common::parse_from_file(model_path))} {}
#endif
};

onnx_editor::ONNXModelEditor::ONNXModelEditor(const std::string& model_path)
    : m_model_path{model_path},
      m_pimpl{new ONNXModelEditor::Impl{model_path}, [](Impl* impl) {
                  delete impl;
              }} {}

#if defined(OPENVINO_ENABLE_UNICODE_PATH_SUPPORT) && defined(_WIN32)
onnx_editor::ONNXModelEditor::ONNXModelEditor(const std::wstring& model_path)
    : m_model_path{file_util::wstring_to_string(model_path)},
      m_pimpl{new ONNXModelEditor::Impl{model_path}, [](Impl* impl) {
                  delete impl;
              }} {}
#endif

onnx_editor::ONNXModelEditor::ONNXModelEditor(std::istream& model_stream, const std::string& model_path)
    : m_model_path{model_path},
      m_pimpl{new ONNXModelEditor::Impl{model_stream}, [](Impl* impl) {
                  delete impl;
              }} {}

const std::string& onnx_editor::ONNXModelEditor::model_path() const {
    return m_model_path;
}

void onnx_editor::ONNXModelEditor::serialize(const std::string& out_file_path) const {
    std::ofstream out_file{out_file_path, std::ios::out | std::ios::binary};

    if (!out_file.is_open()) {
        throw ngraph_error("Could not open the file: " + out_file_path);
    };

    if (!m_pimpl->m_model_proto->SerializeToOstream(&out_file)) {
        throw ngraph_error("Could not serialize the model to: " + out_file_path);
    } else {
        out_file.close();
    }
}

void onnx_editor::ONNXModelEditor::set_input_types(const std::map<std::string, element::Type_t>& input_types) {
    auto* onnx_graph = m_pimpl->m_model_proto->mutable_graph();

    for (const auto& input_desc : input_types) {
        auto* onnx_input = find_graph_input(*onnx_graph, input_desc.first);
        if (onnx_input != nullptr) {
            modify_input_type(*onnx_input, input_desc.second);
        } else {
            throw ngraph_error("Could not set a custom element type for input: " + input_desc.first +
                               ". Such input was not found in the original ONNX model.");
        }
    }
}

void onnx_editor::ONNXModelEditor::set_input_shapes(const std::map<std::string, ngraph::PartialShape>& input_shapes) {
    auto* onnx_graph = m_pimpl->m_model_proto->mutable_graph();

    for (const auto& input_desc : input_shapes) {
        auto* onnx_input = find_graph_input(*onnx_graph, input_desc.first);
        if (onnx_input != nullptr) {
            modify_input_shape(*onnx_input, input_desc.second);
        } else {
            throw ngraph_error("Could not set custom shape for input: " + input_desc.first +
                               ". Such input was not found in the original ONNX model.");
        }
    }
}

PartialShape onnx_editor::ONNXModelEditor::get_tensor_shape(const std::string& tensor_name) const {
    const ValueInfoProto* value_info = nullptr;
    const TensorProto* tensor = nullptr;
    const auto onnx_graph = m_pimpl->m_model_proto->mutable_graph();
    InferShapesAutoRelease onnx_shapes(m_pimpl->m_model_proto);
    if (const auto input = find_graph_input(*onnx_graph, tensor_name)) {
        value_info = input;
    } else if (const auto output = find_graph_output(*onnx_graph, tensor_name)) {
        value_info = output;
    } else if (const auto val_info = find_graph_value_info(*onnx_graph, tensor_name)) {
        value_info = val_info;
    } else if (const auto initializer = find_graph_initializer(*onnx_graph, tensor_name)) {
        tensor = initializer;
    } else {
        try {
            onnx_shapes.infer_shapes();
        } catch (const std::exception& e) {
            NGRAPH_WARN << "Cannot replace existing shapes during get_tensor_shape";
            return PartialShape::dynamic();
        }
        auto node_it = std::find_if(std::begin(onnx_graph->value_info()),
                                    std::end(onnx_graph->value_info()),
                                    [&tensor_name](const ValueInfoProto& value_info) -> bool {
                                        return value_info.name() == tensor_name;
                                    });
        if (node_it != std::end(onnx_graph->value_info())) {
            value_info = &(*node_it);
        }
    }
    if (value_info != nullptr) {
        const auto& onnx_tensor_type = value_info->type().tensor_type();
        if (onnx_tensor_type.has_shape()) {
            return onnx_common::to_ng_shape(onnx_tensor_type.shape());
        } else {
            return PartialShape::dynamic();
        }
    } else if (tensor) {
        return PartialShape{Shape{tensor->dims().cbegin(), tensor->dims().cend()}};
    } else {
        throw ngraph_error("The tensor: " + tensor_name + " was not found in the graph");
    }
}

void onnx_editor::ONNXModelEditor::cut_graph_fragment(const std::vector<InputEdge>& inputs,
                                                      const std::vector<OutputEdge>& outputs) {
    if (inputs.empty() && outputs.empty()) {
        return;
    }

    InferShapesAutoRelease onnx_shapes(m_pimpl->m_model_proto);
    onnx_shapes.infer_shapes();

    SubgraphExtractor editor{*(m_pimpl->m_model_proto->mutable_graph())};
    editor.add_new_inputs(inputs);
    editor.add_new_outputs(outputs);
    editor.extract_subgraph(outputs);

    m_pimpl->m_is_mapper_updated = false;
}

std::vector<std::string> onnx_editor::ONNXModelEditor::model_inputs() const {
    const auto& graph = m_pimpl->m_model_proto->graph();
    std::vector<std::string> inputs;
    for (const auto& in : graph.input()) {
        // ignore inputs which are initializers
        if (std::find_if(graph.initializer().begin(), graph.initializer().end(), [&in](const TensorProto& initializer) {
                return initializer.name() == in.name();
            }) == graph.initializer().end()) {
            inputs.push_back(in.name());
        }
    }
    return inputs;
}

std::vector<std::string> onnx_editor::ONNXModelEditor::model_outputs() const {
    const auto& graph = m_pimpl->m_model_proto->graph();
    std::vector<std::string> outputs;
    outputs.reserve(graph.output_size());

    std::transform(graph.output().begin(),
                   graph.output().end(),
                   std::back_inserter(outputs),
                   extract_name<ONNX_NAMESPACE::ValueInfoProto>);

    return outputs;
}

std::string onnx_editor::ONNXModelEditor::get_source_tensor_name(const InputEdge& edge) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.get_source_tensor_name(edge);
}

bool onnx_editor::ONNXModelEditor::is_input(const InputEdge& edge) const {
    const auto& tensor_name = get_source_tensor_name(edge);
    if (tensor_name.empty()) {
        return false;
    } else {
        const auto& inputs = model_inputs();
        return std::count(std::begin(inputs), std::end(inputs), tensor_name) > 0;
    }
}

std::string onnx_editor::ONNXModelEditor::get_target_tensor_name(const OutputEdge& edge) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.get_target_tensor_name(edge);
}

bool onnx_editor::ONNXModelEditor::is_output(const OutputEdge& edge) const {
    const auto& tensor_name = get_target_tensor_name(edge);
    if (tensor_name.empty()) {
        return false;
    } else {
        const auto& outputs = model_outputs();
        return std::count(std::begin(outputs), std::end(outputs), tensor_name) > 0;
    }
}

std::string onnx_editor::ONNXModelEditor::model_string() const {
    return m_pimpl->m_model_proto->SerializeAsString();
}

std::shared_ptr<Function> onnx_editor::ONNXModelEditor::get_function() const {
    return onnx_import::detail::import_onnx_model(m_pimpl->m_model_proto, m_model_path);
}

void onnx_editor::ONNXModelEditor::set_input_values(
    const std::map<std::string, std::shared_ptr<ngraph::op::Constant>>& input_values) {
    auto onnx_graph = m_pimpl->m_model_proto->mutable_graph();

    for (const auto& input : input_values) {
        auto& name = input.first;
        auto& values = input.second;

        auto onnx_input = find_graph_input(*onnx_graph, name);
        auto onnx_initializer = find_graph_initializer(*onnx_graph, name);

        if (!onnx_initializer && !onnx_input) {
            NGRAPH_INFO << "There is no input nor initializer named '" << name << "' in original model '"
                        << m_model_path << "'.";
        }

        if (!onnx_initializer) {
            onnx_initializer = onnx_graph->add_initializer();
        }

        modify_initializer(*onnx_initializer, name, values, onnx_input);
    }
}

void onnx_editor::ONNXModelEditor::set_tensor_name(const std::string& current_name, const std::string& new_name) {
    OPENVINO_ASSERT(!new_name.empty(), "New name must not be empty.");

    const auto graph = m_pimpl->m_model_proto->mutable_graph();

    OPENVINO_ASSERT(!(find_graph_input(*graph, new_name) || find_graph_output(*graph, new_name) ||
                      find_graph_initializer(*graph, new_name) || find_graph_value_info(*graph, new_name) ||
                      m_pimpl->m_edge_mapper.is_correct_tensor_name(new_name)),
                    "The name '",
                    new_name,
                    "' is already used by another tensor.");

    m_pimpl->m_is_mapper_updated = false;

    // the same tensor can be multiplied in any or all of below arrays
    if (const auto initializer = find_graph_initializer(*graph, current_name))
        *initializer->mutable_name() = new_name;
    if (const auto input = find_graph_input(*graph, current_name))
        *input->mutable_name() = new_name;
    if (const auto output = find_graph_output(*graph, current_name))
        *output->mutable_name() = new_name;
    if (const auto value_info = find_graph_value_info(*graph, current_name))
        *value_info->mutable_name() = new_name;

    for (size_t i = 0; i < graph->node().size(); ++i) {
        const auto node = graph->mutable_node(i);

        bool output_found = false;
        for (size_t j = 0; j < node->output().size(); ++j)
            if (node->output(j) == current_name) {
                *node->mutable_output(j) = new_name;
                output_found = true;
                break;
            }
        if (output_found)
            continue;

        for (size_t j = 0; j < node->input().size(); ++j)
            if (node->input(j) == current_name)
                *node->mutable_input(j) = new_name;
    }
}

void onnx_editor::ONNXModelEditor::set_node_name(const EditorNode& node, const std::string& new_name) {
    const auto node_idx = m_pimpl->m_edge_mapper.get_node_index(node);
    const auto graph = m_pimpl->m_model_proto->mutable_graph();

    m_pimpl->m_is_mapper_updated = false;

    *graph->mutable_node(node_idx)->mutable_name() = new_name;
}

void onnx_editor::ONNXModelEditor::clear_nodes_name(const std::string& name) {
    const auto graph = m_pimpl->m_model_proto->mutable_graph();

    m_pimpl->m_is_mapper_updated = false;

    for (size_t i = 0; i < graph->node().size(); ++i) {
        const auto node = graph->mutable_node(i);
        if (node->has_name() && node->name() == name)
            node->clear_name();
    }
}

void onnx_editor::ONNXModelEditor::set_name_for_dimension(const std::string& node_name,
                                                          size_t shape_dim_index,
                                                          const std::string& dim_name) {
    OPENVINO_ASSERT(!dim_name.empty(), "Dimension name must not be empty.");

    const auto graph = m_pimpl->m_model_proto->mutable_graph();

    OPENVINO_ASSERT(!find_graph_initializer(*graph, node_name), "ONNX initializer shape dimension cannot be dynamic.");

    // the same tensor can be multiplied in any or all of below arrays
    const auto input = find_graph_input(*graph, node_name);
    const auto output = find_graph_output(*graph, node_name);
    const auto value_info = find_graph_value_info(*graph, node_name);
    OPENVINO_ASSERT(input || output || value_info, "There is no tensor named '", node_name, "' in the graph.");

    const auto set_dim_param = [&shape_dim_index, &dim_name](ValueInfoProto* tensor) {
        const auto shape = tensor->mutable_type()->mutable_tensor_type()->mutable_shape();
        auto shape_dim_size = shape->dim_size();

        for (; shape_dim_size <= shape_dim_index; ++shape_dim_size)
            add_dim_to_onnx_shape(Dimension::dynamic(), *shape);

        shape->mutable_dim(shape_dim_index)->set_dim_param(dim_name.c_str());
    };

    m_pimpl->m_is_mapper_updated = false;

    if (input)
        set_dim_param(input);
    if (output)
        set_dim_param(output);
    if (value_info)
        set_dim_param(value_info);
}

void onnx_editor::ONNXModelEditor::update_mapper_if_needed() const {
    if (!m_pimpl->m_is_mapper_updated) {
        m_pimpl->m_edge_mapper = EdgeMapper(m_pimpl->m_model_proto->graph());
    }
    m_pimpl->m_is_mapper_updated = true;
}

InputEdge onnx_editor::ONNXModelEditor::find_input_edge(const EditorNode& node, const EditorInput& input) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.find_input_edge(node, input);
}

OutputEdge onnx_editor::ONNXModelEditor::find_output_edge(const EditorNode& node, const EditorOutput& input) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.find_output_edge(node, input);
}

OutputEdge onnx_editor::ONNXModelEditor::find_output_edge(const std::string& output_name) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.find_output_edge(output_name);
}

std::vector<InputEdge> onnx_editor::ONNXModelEditor::find_output_consumers(const std::string& output_name) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.find_output_consumers(output_name);
}

bool onnx_editor::ONNXModelEditor::is_correct_and_unambiguous_node(const EditorNode& node) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.is_correct_and_unambiguous_node(node);
}

int onnx_editor::ONNXModelEditor::get_node_index(const EditorNode& node) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.get_node_index(node);
}

bool onnx_editor::ONNXModelEditor::is_correct_tensor_name(const std::string& name) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.is_correct_tensor_name(name);
}

std::vector<std::string> onnx_editor::ONNXModelEditor::get_input_ports(const EditorNode& node) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.get_input_ports(node);
}

std::vector<std::string> onnx_editor::ONNXModelEditor::get_output_ports(const EditorNode& node) const {
    update_mapper_if_needed();
    return m_pimpl->m_edge_mapper.get_output_ports(node);
}

std::shared_ptr<Function> onnx_editor::ONNXModelEditor::decode() {
    return onnx_import::detail::decode_to_framework_nodes(m_pimpl->m_model_proto, m_model_path);
}
