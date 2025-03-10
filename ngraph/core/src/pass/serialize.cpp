// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "openvino/pass/serialize.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <ngraph/variant.hpp>
#include <unordered_map>
#include <unordered_set>

#include "itt.hpp"
#include "ngraph/ops.hpp"
#include "ngraph/opsets/opset.hpp"
#include "ngraph/opsets/opset1.hpp"
#include "openvino/op/util/framework_node.hpp"
#include "openvino/pass/constant_folding.hpp"
#include "pugixml.hpp"
#include "transformations/hash.hpp"

using namespace ngraph;

namespace {  // helpers
template <typename Container>
std::string join(const Container& c, const char* glue = ", ") {
    std::stringstream oss;
    const char* s = "";
    for (const auto& v : c) {
        oss << s << v;
        s = glue;
    }
    return oss.str();
}

struct Edge {
    int from_layer = 0;
    int from_port = 0;
    int to_layer = 0;
    int to_port = 0;
};

// Here operation type names are translated from ngraph convention to IR
// convention. Most of them are the same, but there are exceptions, e.g
// Constant (ngraph name) and Const (IR name). If there will be more
// discrepancies discovered, translations needs to be added here.
const std::unordered_map<std::string, std::string> translate_type_name_translator = {{"Constant", "Const"},
                                                                                     {"PRelu", "PReLU"},
                                                                                     {"Relu", "ReLU"},
                                                                                     {"Softmax", "SoftMax"}};

std::string translate_type_name(const std::string& name) {
    auto found = translate_type_name_translator.find(name);
    if (found != end(translate_type_name_translator)) {
        return found->second;
    }
    return name;
}

size_t hash_combine(const void* v, int64_t size) {
    constexpr auto cel_size = sizeof(size_t);
    auto seed = static_cast<size_t>(size);
    const auto data = static_cast<const size_t*>(v);
    const auto d_end = std::next(data, size / cel_size);
    // The constant value used as a magic number has been
    // traditionally used e.g. in boost library's hash_combine.
    // It happens to be derived from the golden ratio.
    for (auto d = data; d != d_end; ++d) {
        seed ^= *d + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    size_t last_bytes{0};
    std::memcpy(&last_bytes, d_end, size % cel_size);
    seed ^= last_bytes + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

class ConstantWriter {
public:
    using FilePosition = int64_t;
    using HashValue = size_t;
    using ConstWritePositions = std::unordered_map<HashValue, std::pair<FilePosition, void const*>>;

    ConstantWriter(std::ostream& bin_data, bool enable_compression = true)
        : m_binary_output(bin_data),
          m_enable_compression(enable_compression),
          m_blob_offset(bin_data.tellp()) {}

    FilePosition write(const char* ptr, size_t size) {
        const FilePosition write_pos = m_binary_output.tellp();
        const auto offset = write_pos - m_blob_offset;
        if (!m_enable_compression) {
            m_binary_output.write(ptr, size);
            return offset;
        }
        // This hash is weak (but efficient) and must be replace with some other
        // more stable hash algorithm. For example current hash algorithms gives
        // the same hash for {2, 2} and {0, 128} arrays. So we have to compare
        // values when finding a match in hash map.
        const HashValue hash = hash_combine(ptr, size);
        const auto found = m_hash_to_file_positions.find(hash);
        if (found != end(m_hash_to_file_positions) &&
            memcmp(static_cast<void const*>(ptr), found->second.second, size) == 0) {
            return found->second.first;
        }

        m_binary_output.write(ptr, size);
        m_hash_to_file_positions.insert({hash, {offset, static_cast<void const*>(ptr)}});

        return offset;
    }

private:
    ConstWritePositions m_hash_to_file_positions;
    std::ostream& m_binary_output;
    bool m_enable_compression;
    FilePosition m_blob_offset;  // blob offset inside output stream
};

void ngfunction_2_ir(pugi::xml_node& node,
                     const ngraph::Function& f,
                     const std::map<std::string, ngraph::OpSet>& custom_opsets,
                     ConstantWriter& constant_write_handler,
                     int64_t version,
                     bool deterministic);

namespace rt_info {
const std::vector<std::string> list_of_names{
    "PrimitivesPriority",
    "alt_width",
};

class XmlSerializer {
public:
    explicit XmlSerializer(pugi::xml_node& xml_node) : m_xml_node(xml_node) {}

    void serialize(const ngraph::Node::RTMap& rt_info) {
        for (const auto& rt_info_name : list_of_names) {
            const auto& found_rt_info = rt_info.find(rt_info_name);
            if (found_rt_info != rt_info.end()) {
                xml_node_append_attribute<std::string>(rt_info_name, found_rt_info->second);
            }
        }
    }

private:
    template <typename VariantType>
    void xml_node_append_attribute(const std::string& name, const std::shared_ptr<ngraph::Variant>& variant) {
        if (auto v = std::dynamic_pointer_cast<ngraph::VariantImpl<VariantType>>(variant)) {
            const auto& value = v->get();
            m_xml_node.append_attribute(name.c_str()).set_value(value.c_str());
        }
    }

    pugi::xml_node& m_xml_node;
};

class RTInfoSerializer : public ngraph::AttributeVisitor {
    pugi::xml_node m_node;

public:
    RTInfoSerializer(const pugi::xml_node node) : m_node(node) {}

    void on_adapter(const std::string& name, ngraph::ValueAccessor<void>& adapter) override {
        check_attribute_name(name);
        if (auto a = ov::as_type<ov::AttributeAdapter<std::set<std::string>>>(&adapter)) {
            const auto& value = join(a->get());
            m_node.append_attribute(name.c_str()).set_value(value.c_str());
        } else {
            throw ngraph_error("Unsupported attribute type for serialization: " + name);
        }
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<bool>& adapter) override {
        check_attribute_name(name);
        m_node.append_attribute(name.c_str()).set_value(adapter.get());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::string>& adapter) override {
        check_attribute_name(name);
        m_node.append_attribute(name.c_str()).set_value(adapter.get().c_str());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<int64_t>& adapter) override {
        check_attribute_name(name);
        m_node.append_attribute(name.c_str()).set_value(adapter.get());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<double>& adapter) override {
        check_attribute_name(name);
        m_node.append_attribute(name.c_str()).set_value(adapter.get());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<int>>& adapter) override {
        check_attribute_name(name);
        const auto& value = join(adapter.get());
        m_node.append_attribute(name.c_str()).set_value(value.c_str());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<int64_t>>& adapter) override {
        check_attribute_name(name);
        const auto& value = join(adapter.get());
        m_node.append_attribute(name.c_str()).set_value(value.c_str());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<uint64_t>>& adapter) override {
        check_attribute_name(name);
        const auto& value = join(adapter.get());
        m_node.append_attribute(name.c_str()).set_value(value.c_str());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<float>>& adapter) override {
        check_attribute_name(name);
        const auto& value = join(adapter.get());
        m_node.append_attribute(name.c_str()).set_value(value.c_str());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<std::string>>& adapter) override {
        check_attribute_name(name);
        const auto& value = join(adapter.get());
        m_node.append_attribute(name.c_str()).set_value(value.c_str());
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::shared_ptr<Function>>& adapter) override {
        throw ngraph_error("Function type is unsupported for rt info serialization");
    }

    void check_attribute_name(const std::string& name) const {
        if (name == "name" || name == "version") {
            throw ngraph_error("Attribute key with name: " + name + " is not allowed. Please use another name");
        }
    }
};
}  // namespace rt_info

class XmlSerializer : public ngraph::AttributeVisitor {
    pugi::xml_node& m_xml_node;
    const std::string& m_node_type_name;
    const std::map<std::string, ngraph::OpSet>& m_custom_opsets;
    ConstantWriter& m_constant_write_handler;
    int64_t m_version;
    bool m_deterministic;

    template <typename T>
    std::string create_atribute_list(ngraph::ValueAccessor<std::vector<T>>& adapter) {
        return join(adapter.get());
    }

    std::vector<std::string> map_type_from_body(const pugi::xml_node& xml_node,
                                                const std::string& map_type,
                                                int64_t ir_version,
                                                const std::string& body_name = "body") {
        std::vector<std::string> output;
        for (pugi::xml_node node : xml_node.child(body_name.c_str()).child("layers")) {
            if (map_type == node.attribute("type").value()) {
                output.emplace_back(node.attribute("id").value());
            }
        }

        if (ir_version < 11) {
            // ops for serialized body function are provided in reversed order
            std::reverse(output.begin(), output.end());
        }

        return output;
    }

    void input_descriptions_on_adapter(
        const std::vector<std::shared_ptr<ngraph::op::util::MultiSubGraphOp::InputDescription>>& input_descriptions,
        const std::vector<std::string>& parameter_mapping,
        const std::vector<std::string>& result_mapping,
        pugi::xml_node& port_map,
        const std::string& portmap_name) {
        NGRAPH_CHECK(!parameter_mapping.empty(), "No parameters found in body Function.");

        if (!m_xml_node.parent().child(portmap_name.c_str())) {
            port_map = m_xml_node.parent().insert_child_before(portmap_name.c_str(), m_xml_node.parent().first_child());
        }

        for (const auto& input_description : input_descriptions) {
            pugi::xml_node input = port_map.append_child("input");
            input.append_attribute("external_port_id").set_value(input_description->m_input_index);
            input.append_attribute("internal_layer_id")
                .set_value(parameter_mapping[input_description->m_body_parameter_index].c_str());

            if (auto slice_input =
                    ov::as_type_ptr<ngraph::op::util::SubGraphOp::SliceInputDescription>(input_description)) {
                input.prepend_attribute("axis").set_value(slice_input->m_axis);
                input.append_attribute("start").set_value(slice_input->m_start);
                input.append_attribute("end").set_value(slice_input->m_end);
                input.append_attribute("stride").set_value(slice_input->m_stride);
                input.append_attribute("part_size").set_value(slice_input->m_part_size);
            } else if (auto merged_input =
                           ov::as_type_ptr<ngraph::op::util::SubGraphOp::MergedInputDescription>(input_description)) {
                pugi::xml_node back_edges = m_xml_node.parent().child("back_edges");
                if (!back_edges) {
                    back_edges = m_xml_node.parent().insert_child_after("back_edges", port_map);
                }
                pugi::xml_node edge = back_edges.append_child("edge");
                edge.append_attribute("from-layer").set_value(result_mapping[merged_input->m_body_value_index].c_str());
                edge.append_attribute("to-layer")
                    .set_value(parameter_mapping[merged_input->m_body_parameter_index].c_str());
            }
        }
    }

    void output_descriptions_on_adapter(
        const std::vector<std::shared_ptr<ngraph::op::util::MultiSubGraphOp::OutputDescription>>& output_descriptions,
        const uint32_t& input_count,
        const std::vector<std::string>& result_mapping,
        pugi::xml_node& port_map,
        const std::string& portmap_name) {
        NGRAPH_CHECK(!result_mapping.empty(), "No results found in body Function.");

        if (!port_map) {
            port_map = m_xml_node.parent().insert_child_before(portmap_name.c_str(), m_xml_node.parent().first_child());
        }

        for (const auto& output_description : output_descriptions) {
            pugi::xml_node output = port_map.append_child("output");
            output.append_attribute("external_port_id").set_value(input_count + output_description->m_output_index);
            output.append_attribute("internal_layer_id")
                .set_value(result_mapping[output_description->m_body_value_index].c_str());

            if (auto concat_output =
                    ov::as_type_ptr<ngraph::op::util::SubGraphOp::ConcatOutputDescription>(output_description)) {
                output.prepend_attribute("axis").set_value(concat_output->m_axis);
                output.append_attribute("start").set_value(concat_output->m_start);
                output.append_attribute("end").set_value(concat_output->m_end);
                output.append_attribute("stride").set_value(concat_output->m_stride);
                output.append_attribute("part_size").set_value(concat_output->m_part_size);
            }
        }
    }

    void special_body_ports_on_adapter(const ngraph::op::v5::Loop::SpecialBodyPorts& special_body_ports,
                                       const std::vector<std::string>& parameter_mapping,
                                       const std::vector<std::string>& result_mapping,
                                       pugi::xml_node& port_map) {
        NGRAPH_CHECK(port_map, "port_map section not found, purpose attribute cannot be added.");

        if (special_body_ports.current_iteration_input_idx != -1) {
            pugi::xml_node iter_input = port_map.append_child("input");
            iter_input.append_attribute("external_port_id").set_value("-1");
            iter_input.append_attribute("internal_layer_id")
                .set_value(parameter_mapping[special_body_ports.current_iteration_input_idx].c_str());
            iter_input.append_attribute("purpose").set_value("current_iteration");
        }

        if (special_body_ports.body_condition_output_idx != -1) {
            pugi::xml_node exec_output = port_map.append_child("output");
            exec_output.append_attribute("external_port_id").set_value("-1");
            exec_output.append_attribute("internal_layer_id")
                .set_value(result_mapping[special_body_ports.body_condition_output_idx].c_str());
            exec_output.append_attribute("purpose").set_value("execution_condition");
        }
    }

public:
    XmlSerializer(pugi::xml_node& data,
                  const std::string& node_type_name,
                  const std::map<std::string, ngraph::OpSet>& custom_opsets,
                  ConstantWriter& constant_write_handler,
                  int64_t version,
                  bool deterministic = false)
        : m_xml_node(data),
          m_node_type_name(node_type_name),
          m_custom_opsets(custom_opsets),
          m_constant_write_handler(constant_write_handler),
          m_version(version),
          m_deterministic(deterministic) {}

    void on_adapter(const std::string& name, ngraph::ValueAccessor<void>& adapter) override {
        using BodyTargetNames = std::tuple<std::string, std::string, std::vector<std::string>>;

        const std::vector<BodyTargetNames> body_names = {
            BodyTargetNames{"body", "port_map", {"input_descriptions", "output_descriptions", "special_body_ports"}},
            BodyTargetNames{"then_body", "then_port_map", {"then_inputs", "then_outputs"}},
            BodyTargetNames{"else_body", "else_port_map", {"else_inputs", "else_outputs"}}};
        BodyTargetNames bnames;
        bool is_body_target = false;
        for (const auto& _body_target : body_names) {
            if (m_xml_node.parent().child(std::get<0>(_body_target).c_str())) {
                auto vec_names = std::get<2>(_body_target);

                if (std::find(vec_names.begin(), vec_names.end(), name) != vec_names.end()) {
                    is_body_target = true;
                    bnames = _body_target;
                    break;
                }
            }
        }
        if (is_body_target) {
            auto body_name = std::get<0>(bnames);
            auto portmap_name = std::get<1>(bnames);
            std::vector<std::string> result_mapping =
                map_type_from_body(m_xml_node.parent(), "Result", m_version, body_name);
            std::vector<std::string> parameter_mapping =
                map_type_from_body(m_xml_node.parent(), "Parameter", m_version, body_name);

            pugi::xml_node port_map = m_xml_node.parent().child(portmap_name.c_str());

            NGRAPH_CHECK(!parameter_mapping.empty() || !result_mapping.empty(),
                         "No parameters or results found in body Function.");
            // TI, Loop do not have attributtes as regular ops, it is necessary to append "port_map" and
            // "back_edges" to layer above (m_xml_node.parent()) as in ngfunction_2_ir() layer (here "m_xml_node")
            // with empty attributes is removed.
            if (const auto& a = ngraph::as_type<ngraph::AttributeAdapter<
                    std::vector<std::shared_ptr<ngraph::op::util::MultiSubGraphOp::InputDescription>>>>(&adapter)) {
                input_descriptions_on_adapter(a->get(), parameter_mapping, result_mapping, port_map, portmap_name);
            } else if (const auto& a = ngraph::as_type<ngraph::AttributeAdapter<
                           std::vector<std::shared_ptr<ngraph::op::util::MultiSubGraphOp::OutputDescription>>>>(
                           &adapter)) {
                uint32_t op_input_count = 0;
                for (auto c = m_xml_node.parent().child("input").first_child(); !c.empty(); c = c.next_sibling()) {
                    op_input_count++;
                }
                output_descriptions_on_adapter(a->get(), op_input_count, result_mapping, port_map, portmap_name);
            } else if (const auto& a =
                           ngraph::as_type<ngraph::AttributeAdapter<ngraph::op::v5::Loop::SpecialBodyPorts>>(
                               &adapter)) {
                special_body_ports_on_adapter(a->get(), parameter_mapping, result_mapping, port_map);
            }
        } else if (const auto& a =
                       ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::Variable>>>(&adapter)) {
            m_xml_node.append_attribute(name.c_str()).set_value(a->get()->get_info().variable_id.c_str());
        } else if (const auto& a =
                       ngraph::as_type<ngraph::AttributeAdapter<std::shared_ptr<ngraph::runtime::AlignedBuffer>>>(
                           &adapter)) {
            if (name == "value" && translate_type_name(m_node_type_name) == "Const") {
                const int64_t size = a->get()->size();
                int64_t offset = m_constant_write_handler.write(static_cast<const char*>(a->get()->get_ptr()), size);

                m_xml_node.append_attribute("offset").set_value(offset);
                m_xml_node.append_attribute("size").set_value(size);
            }
        } else if (const auto& a =
                       ngraph::as_type<ngraph::AttributeAdapter<ov::op::util::FrameworkNodeAttrs>>(&adapter)) {
            const auto& attrs = a->get();

            // Update type and version attributes
            pugi::xml_node layer = m_xml_node.parent();

            auto type_attr = layer.attribute("type");
            auto version_attr = layer.attribute("version");

            type_attr.set_value(attrs.get_type_name().c_str());

            if (!attrs.get_opset_name().empty()) {
                version_attr.set_value(attrs.get_opset_name().c_str());
            } else {
                layer.remove_attribute("version");
            }

            // Update node attributes in data field
            for (const auto& attr : attrs) {
                m_xml_node.append_attribute(attr.first.c_str()).set_value(attr.second.c_str());
            }
        } else if (const auto& a = ngraph::as_type<ngraph::AttributeAdapter<ngraph::element::TypeVector>>(&adapter)) {
            const auto& attrs = a->get();
            m_xml_node.append_attribute(name.c_str()).set_value(join(attrs).c_str());
        } else {
            throw ngraph_error("Unsupported attribute type for serialization: " + name);
        }
    }

    void on_adapter(const std::string& name, ngraph::ValueAccessor<bool>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::string>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get().c_str());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<int64_t>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<double>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(adapter.get());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<int>>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<int64_t>>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<uint64_t>>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<float>>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::vector<std::string>>& adapter) override {
        m_xml_node.append_attribute(name.c_str()).set_value(create_atribute_list(adapter).c_str());
    }
    void on_adapter(const std::string& name, ngraph::ValueAccessor<std::shared_ptr<Function>>& adapter) override {
        if (name == "body" || name == "then_body" || name == "else_body") {
            // TI, Loop do not have attributtes as regular ops, it is necessary to append "body"
            // to layer above (m_xml_node.parent()) as in ngfunction_2_ir() layer (m_xml_node) with empty attributes
            // is removed.
            pugi::xml_node xml_body = m_xml_node.parent().append_child(name.c_str());
            ngfunction_2_ir(xml_body,
                            *adapter.get(),
                            m_custom_opsets,
                            m_constant_write_handler,
                            m_version,
                            m_deterministic);
            xml_body.remove_attribute("name");
            xml_body.remove_attribute("version");
        } else if (name == "net") {
            ngfunction_2_ir(m_xml_node,
                            *adapter.get(),
                            m_custom_opsets,
                            m_constant_write_handler,
                            m_version,
                            m_deterministic);
        } else {
            NGRAPH_CHECK(false, "Unsupported Function name.");
        }
    }
};

const std::unordered_map<ngraph::Node*, int> create_layer_ids(const ngraph::Function& f) {
    std::unordered_map<ngraph::Node*, int> layer_ids;
    int id = 0;
    for (const auto& node : f.get_ordered_ops()) {
        layer_ids[node.get()] = id++;
    }
    return layer_ids;
}

const std::vector<Edge> create_edge_mapping(const std::unordered_map<ngraph::Node*, int>& layer_ids,
                                            const ngraph::Function& f) {
    std::vector<Edge> edges;
    for (const auto& node : f.get_ordered_ops()) {
        if (ngraph::op::is_parameter(node)) {
            continue;
        }

        for (const auto& i : node->inputs()) {
            auto source_output = i.get_source_output();
            auto source_node = source_output.get_node();
            auto current_node = i.get_node();

            NGRAPH_CHECK(layer_ids.find(source_node) != layer_ids.end(), "Internal error");
            NGRAPH_CHECK(layer_ids.find(current_node) != layer_ids.end(), "Internal error");

            Edge e{};
            e.from_layer = layer_ids.find(source_node)->second;
            e.from_port = source_node->get_input_size() + source_output.get_index();
            e.to_layer = layer_ids.find(current_node)->second;
            e.to_port = i.get_index();
            edges.push_back(e);
        }
    }
    std::sort(begin(edges), end(edges), [](const Edge& a, const Edge& b) -> bool {
        return a.from_layer < b.from_layer;
    });
    return edges;
}

std::string get_opset_name(const ngraph::Node* n, const std::map<std::string, ngraph::OpSet>& custom_opsets) {
    OPENVINO_ASSERT(n != nullptr);
    if (n->get_type_info().version_id != nullptr) {
        return n->get_type_info().version_id;
    }
    // Try to find opset name from RT info
    auto opset_it = n->get_rt_info().find("opset");
    if (opset_it != n->get_rt_info().end()) {
        if (auto variant = std::dynamic_pointer_cast<ngraph::VariantImpl<std::string>>(opset_it->second)) {
            const std::string& opset_name = variant->get();
            if (custom_opsets.find(opset_name) != custom_opsets.end()) {
                return opset_name;
            }
        }
    }

    for (const auto& custom_opset : custom_opsets) {
        std::string name = custom_opset.first;
        ngraph::OpSet opset = custom_opset.second;
        if (opset.contains_op_type(n)) {
            return name;
        }
    }

    return "experimental";
}

std::string get_precision_name(const ngraph::element::Type& elem_type) {
    switch (elem_type) {
    case ::ngraph::element::Type_t::undefined:
    case ::ngraph::element::Type_t::dynamic:
        return "UNSPECIFIED";
    case ::ngraph::element::Type_t::f16:
        return "FP16";
    case ::ngraph::element::Type_t::f32:
        return "FP32";
    case ::ngraph::element::Type_t::bf16:
        return "BF16";
    case ::ngraph::element::Type_t::f64:
        return "FP64";
    case ::ngraph::element::Type_t::i4:
        return "I4";
    case ::ngraph::element::Type_t::i8:
        return "I8";
    case ::ngraph::element::Type_t::i16:
        return "I16";
    case ::ngraph::element::Type_t::i32:
        return "I32";
    case ::ngraph::element::Type_t::i64:
        return "I64";
    case ::ngraph::element::Type_t::u4:
        return "U4";
    case ::ngraph::element::Type_t::u8:
        return "U8";
    case ::ngraph::element::Type_t::u16:
        return "U16";
    case ::ngraph::element::Type_t::u32:
        return "U32";
    case ::ngraph::element::Type_t::u64:
        return "U64";
    case ::ngraph::element::Type_t::u1:
        return "BIN";
    case ::ngraph::element::Type_t::boolean:
        return "BOOL";
    default:
        std::stringstream msg;
        msg << "Unsupported precision: " << elem_type;
        throw ngraph_error(msg.str());
    }
}

std::string escape_delim(const std::string& name, const char delim = ',') {
    std::string result_name = name;
    const std::string escaped_delim = std::string("\\") + delim;
    size_t index = result_name.find(delim, 0);
    while (index != std::string::npos) {
        result_name.replace(index, 1, escaped_delim);
        index = result_name.find(delim, index + 2);
    }
    return result_name;
}

std::string generate_unique_name(const std::unordered_set<std::string>& unique_names,
                                 const std::string& base_name,
                                 int suffix) {
    std::string new_name = base_name + std::to_string(suffix);
    if (unique_names.find(new_name) == unique_names.end()) {
        return new_name;
    } else {
        suffix++;
        return generate_unique_name(unique_names, base_name, suffix);
    }
}

template <typename T>
bool is_name_auto_generated(const T& n) {
    return n.get_friendly_name() == n.get_name();
}

// TODO: remove when CNNNetwork will be supporting not-unique names
std::string get_node_unique_name(std::unordered_set<std::string>& unique_names, const ngraph::Node* n) {
    std::string name = n->get_friendly_name();
    if (unique_names.find(name) != unique_names.end()) {
        name = generate_unique_name(unique_names, name, 0);
    }
    unique_names.insert(name);
    return name;
}

void visit_exec_graph_node(pugi::xml_node& layer, const ngraph::Node* n) {
    auto data = layer.child("data");
    for (const auto& param : n->get_rt_info()) {
        if (auto variant = std::dynamic_pointer_cast<ngraph::VariantImpl<std::string>>(param.second)) {
            const std::string& name = param.first;
            const std::string& value = variant->get();

            if (name == "layerType") {
                layer.attribute("type").set_value(value.c_str());
                continue;
            }

            data.append_attribute(name.c_str()).set_value(value.c_str());
        }
    }
}

bool is_exec_graph(const ngraph::Function& f) {
    // go over all operations and check whether performance stat is set
    for (const auto& op : f.get_ops()) {
        const auto& rtInfo = op->get_rt_info();
        if (rtInfo.find("execTimeMcs") != rtInfo.end()) {
            return true;
        }
    }
    return false;
}

bool has_dynamic_output(const std::shared_ptr<Node>& n) {
    for (size_t i = 0; i < n->get_output_size(); i++) {
        if (n->get_output_partial_shape(i).is_dynamic()) {
            return true;
        }
    }
    return false;
}

bool resolve_dynamic_shapes(const ngraph::Function& f) {
    const auto& f_ops = f.get_ordered_ops();
    if (std::all_of(f_ops.begin(), f_ops.end(), [](const std::shared_ptr<Node>& results) {
            return !results->is_dynamic() && !has_dynamic_output(results);
        })) {
        return false;
    }

    auto f_clone = ngraph::clone_function(f);
    const auto& f_clone_ops = f_clone->get_ordered_ops();
    NGRAPH_CHECK(f_ops.size() == f_clone_ops.size(), "Unexpected get_ordered_ops method behaviour");

    for (size_t id = 0; id < f_ops.size(); ++id) {
        auto& op = f_ops[id];
        auto& clone_op = f_clone_ops[id];
        ov::pass::enable_constant_folding(clone_op);  // to be able to fold ShapeOfs
        if (auto op_subgraph = std::dynamic_pointer_cast<ngraph::op::util::SubGraphOp>(op)) {
            resolve_dynamic_shapes(*op_subgraph->get_function());
        }

        op->validate_and_infer_types();
        clone_op->validate_and_infer_types();

        // dynamic_to_static function converts dynamic dimensions to static using
        // upperbound (get_max_length) dimension value.
        auto dynamic_to_static = [&op](const PartialShape& shape) -> PartialShape {
            if (shape.is_static() || shape.rank().is_dynamic()) {
                return shape;
            }
            std::vector<Dimension> out_shape;
            std::transform(std::begin(shape),
                           std::end(shape),
                           std::back_inserter(out_shape),
                           [](const Dimension& d) -> Dimension {
                               return d.get_max_length();
                           });
            return out_shape;
        };

        OutputVector replacements(clone_op->get_output_size());
        if (!clone_op->constant_fold(replacements, clone_op->input_values())) {
            for (size_t output_id = 0; output_id < clone_op->get_output_size(); ++output_id) {
                clone_op->set_output_type(output_id,
                                          clone_op->output(output_id).get_element_type(),
                                          dynamic_to_static(clone_op->output(output_id).get_partial_shape()));
                op->set_output_type(output_id,
                                    clone_op->output(output_id).get_element_type(),
                                    clone_op->output(output_id).get_partial_shape());
            }
        } else {
            for (size_t output_id = 0; output_id < clone_op->get_output_size(); ++output_id) {
                op->set_output_type(output_id,
                                    replacements[output_id].get_element_type(),
                                    replacements[output_id].get_partial_shape());
            }

            for (size_t i = 0; i < replacements.size(); ++i) {
                auto node_output = clone_op->output(i);
                auto replacement = replacements.at(i);
                if (replacement.get_node_shared_ptr() && (node_output != replacement)) {
                    node_output.replace(replacement);
                }
            }
        }
    }
    return true;
}

void auto_pad_resolving(ov::Node* node) {
    const std::set<ov::op::PadType> pad_agnostic_types = {
        ov::op::PadType::SAME_LOWER,
        ov::op::PadType::SAME_UPPER,
        ov::op::PadType::VALID,
        ov::op::PadType::AUTO,
    };
    if (auto op = as_type<opset1::Convolution>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(CoordinateDiff(op->get_pads_begin().size(), 0));
            op->set_adding_above(CoordinateDiff(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<opset1::GroupConvolution>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(CoordinateDiff(op->get_pads_begin().size(), 0));
            op->set_adding_above(CoordinateDiff(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<opset1::ConvolutionBackpropData>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(CoordinateDiff(op->get_pads_begin().size(), 0));
            op->set_pads_end(CoordinateDiff(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<opset1::GroupConvolutionBackpropData>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(CoordinateDiff(op->get_pads_begin().size(), 0));
            op->set_pads_end(CoordinateDiff(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<ngraph::op::util::DeformableConvolutionBase>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(CoordinateDiff(op->get_pads_begin().size(), 0));
            op->set_pads_end(CoordinateDiff(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<opset1::BinaryConvolution>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(CoordinateDiff(op->get_pads_begin().size(), 0));
            op->set_adding_above(CoordinateDiff(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<opset1::AvgPool>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(Shape(op->get_pads_begin().size(), 0));
            op->set_pads_end(Shape(op->get_pads_end().size(), 0));
        }
    } else if (auto op = as_type<ngraph::op::util::MaxPoolBase>(node)) {
        if (pad_agnostic_types.count(op->get_auto_pad())) {
            op->set_pads_begin(Shape(op->get_pads_begin().size(), 0));
            op->set_adding_above(Shape(op->get_pads_end().size(), 0));
        }
    }
}

void ngfunction_2_ir(pugi::xml_node& netXml,
                     const ngraph::Function& f,
                     const std::map<std::string, ngraph::OpSet>& custom_opsets,
                     ConstantWriter& constant_node_write_handler,
                     int64_t version,
                     bool deterministic) {
    // If determinism is not required, include auto-generated names into xml
    if (!deterministic || !is_name_auto_generated(f)) {
        netXml.append_attribute("name").set_value(f.get_friendly_name().c_str());
    }
    netXml.append_attribute("version").set_value(version);
    pugi::xml_node layers = netXml.append_child("layers");

    const std::unordered_map<ngraph::Node*, int> layer_ids = create_layer_ids(f);
    std::unordered_set<std::string> unique_names;

    // TODO remove resolve_dynamic_shapes function completely when support for -1 will be implemented in the MO
    bool has_dynamic_shapes = resolve_dynamic_shapes(f);

    const bool exec_graph = is_exec_graph(f);

    auto sorted_ops = f.get_ordered_ops();
    if (version >= 11) {
        std::vector<std::shared_ptr<ov::Node>> result;
        result.reserve(sorted_ops.size());
        for (const auto& param : f.get_parameters()) {
            result.emplace_back(param);
        }
        for (auto&& node : sorted_ops) {
            if (!ov::op::util::is_parameter(node) && !ov::op::util::is_output(node) && !ov::op::util::is_sink(node))
                result.emplace_back(node);
        }
        for (const auto& sink : f.get_sinks()) {
            result.emplace_back(sink);
        }
        for (const auto& res : f.get_results()) {
            result.emplace_back(res);
        }
        sorted_ops = result;
    }

    for (const auto& n : sorted_ops) {
        ngraph::Node* node = n.get();
        const std::string& node_type_name{node->get_type_name()};

        NGRAPH_CHECK(layer_ids.find(node) != layer_ids.end(), "Internal error");
        // <layers>
        pugi::xml_node layer = layers.append_child("layer");
        layer.append_attribute("id").set_value(layer_ids.find(node)->second);
        // If determinism is not required, include auto-generated names into xml
        if (!deterministic || !is_name_auto_generated(*node)) {
            layer.append_attribute("name").set_value(get_node_unique_name(unique_names, node).c_str());
        }
        layer.append_attribute("type").set_value(translate_type_name(node_type_name).c_str());
        if (!exec_graph) {
            layer.append_attribute("version").set_value(get_opset_name(node, custom_opsets).c_str());
        }

        // <layers/data> general attributes
        pugi::xml_node data = layer.append_child("data");

        auto append_runtime_info = [](pugi::xml_node& node, const RTMap& attributes) {
            pugi::xml_node rt_node = node.append_child("rt_info");
            bool has_attrs = false;
            for (const auto& item : attributes) {
                auto attribute_node = rt_node.append_child("attribute");
                attribute_node.append_attribute("name").set_value(item.second->get_type_info().name);
                attribute_node.append_attribute("version").set_value(
                    item.second->get_type_info().get_version().c_str());
                rt_info::RTInfoSerializer serializer(attribute_node);
                if (!item.second->visit_attributes(serializer)) {
                    rt_node.remove_child(attribute_node);
                } else {
                    has_attrs = true;
                }
            }
            if (!has_attrs) {
                node.remove_child(rt_node);
            }
        };

        if (version >= 11) {
            append_runtime_info(layer, node->get_rt_info());
        }

        int port_id = 0;
        // <layers/input>
        if (node->get_input_size() > 0) {
            pugi::xml_node input = layer.append_child("input");
            for (const auto& i : node->inputs()) {
                // WA for LSTMCellv0, peephole input shall not be serialized
                if (i.get_index() == 6 && dynamic_cast<opset1::LSTMCell*>(node)) {
                    port_id++;
                    continue;
                }

                pugi::xml_node port = input.append_child("port");
                port.append_attribute("id").set_value(port_id++);
                port.append_attribute("precision").set_value(get_precision_name(i.get_element_type()).c_str());
                for (auto d : i.get_partial_shape()) {
                    pugi::xml_node dim = port.append_child("dim");
                    if (d.is_dynamic()) {
                        dim.append_child(pugi::xml_node_type::node_pcdata).set_value("-1");
                    } else {
                        dim.append_child(pugi::xml_node_type::node_pcdata)
                            .set_value(std::to_string(d.get_length()).c_str());
                    }
                }
                if (version >= 11)
                    append_runtime_info(port, i.get_rt_info());
            }

            if (node_type_name == "TensorIterator" || node_type_name == "Loop") {
                layer.prepend_move(input);
            }
        }
        // <layers/output>
        if ((node->get_output_size() > 0) && !ngraph::op::is_output(node)) {
            pugi::xml_node output = layer.append_child("output");
            for (const auto& o : node->outputs()) {
                pugi::xml_node port = output.append_child("port");
                port.append_attribute("id").set_value(port_id++);
                port.append_attribute("precision").set_value(get_precision_name(o.get_element_type()).c_str());

                // Sort tensor names
                const auto& tensor_names = o.get_tensor().get_names();
                std::vector<std::string> vector_names(tensor_names.begin(), tensor_names.end());
                sort(vector_names.begin(), vector_names.end());

                std::string names;
                for (const auto& name : vector_names) {
                    if (!names.empty())
                        names += ",";
                    names += escape_delim(name);
                }
                if (!names.empty()) {
                    port.append_attribute("names").set_value(names.c_str());
                }

                for (auto d : o.get_partial_shape()) {
                    pugi::xml_node dim = port.append_child("dim");
                    if (d.is_dynamic()) {
                        dim.append_child(pugi::xml_node_type::node_pcdata).set_value("-1");
                    } else {
                        dim.append_child(pugi::xml_node_type::node_pcdata)
                            .set_value(std::to_string(d.get_length()).c_str());
                    }
                }
                if (version >= 11)
                    append_runtime_info(port, o.get_rt_info());
            }
            if (node_type_name == "TensorIterator" || node_type_name == "Loop") {
                layer.insert_move_after(output, layer.first_child());
            }
        }

        // fill <data> general attributes
        auto_pad_resolving(node);  // Backward compatibility: clear padding values for nodes with auto_pad
        XmlSerializer visitor(data, node_type_name, custom_opsets, constant_node_write_handler, version, deterministic);
        NGRAPH_CHECK(node->visit_attributes(visitor), "Visitor API is not supported in ", node);
        rt_info::XmlSerializer{data}.serialize(node->get_rt_info());

        if (exec_graph) {
            visit_exec_graph_node(layer, node);
        }

        const bool data_attr_size = data.attributes().begin() == data.attributes().end();
        if (data_attr_size) {
            layer.remove_child(data);
        }
    }
    // <edges>
    const std::vector<Edge> edge_mapping = create_edge_mapping(layer_ids, f);
    pugi::xml_node edges = netXml.append_child("edges");
    for (auto e : edge_mapping) {
        // WA for LSTMCellv0, peephole input shall not be serialized
        if (e.to_port == 6) {
            auto type_info = f.get_ordered_ops()[e.to_layer]->get_type_info();
            if (!strcmp(type_info.name, "LSTMCell") && type_info.version == 0) {
                continue;
            }
        }
        pugi::xml_node edge = edges.append_child("edge");
        edge.append_attribute("from-layer").set_value(e.from_layer);
        edge.append_attribute("from-port").set_value(e.from_port);
        edge.append_attribute("to-layer").set_value(e.to_layer);
        edge.append_attribute("to-port").set_value(e.to_port);
    }
    // move back dynamic shapes
    if (has_dynamic_shapes) {
        f.validate_nodes_and_infer_types();
    }
}

std::string valid_xml_path(const std::string& path) {
    NGRAPH_CHECK(path.length() > 4, "Path for xml file is to short: \"" + path + "\"");

    const char* const extension = ".xml";
    const bool has_xml_extension = path.rfind(extension) == path.size() - std::strlen(extension);
    NGRAPH_CHECK(has_xml_extension,
                 "Path for xml file doesn't contains file name with 'xml' extension: \"" + path + "\"");
    return path;
}

std::string provide_bin_path(const std::string& xmlPath, const std::string& binPath) {
    if (!binPath.empty()) {
        return binPath;
    }
    assert(xmlPath.size() > 4);  // should be check by valid_xml_path
    std::string bestPath = xmlPath;
    const char* const extension = "bin";
    const auto ext_size = std::strlen(extension);
    bestPath.replace(bestPath.size() - ext_size, ext_size, extension);
    return bestPath;
}

void serializeFunc(std::ostream& xml_file,
                   std::ostream& bin_file,
                   std::shared_ptr<ov::Function> f,
                   ov::pass::Serialize::Version ver,
                   const std::map<std::string, ngraph::OpSet>& custom_opsets,
                   bool deterministic = false) {
    auto version = static_cast<int64_t>(ver);

    auto& rt_info = f->get_rt_info();
    if (rt_info.count("version")) {
        auto version_var = std::dynamic_pointer_cast<VariantWrapper<int64_t>>(rt_info.at("version"));
        version = version_var->get();
    }

    if (version != static_cast<int64_t>(ver) && ver != ov::pass::Serialize::Version::UNSPECIFIED)
        throw ngraph_error("Cannot serialize function to incompatible IR version");

    if (version == static_cast<int64_t>(ov::pass::Serialize::Version::UNSPECIFIED))
        version = static_cast<int64_t>(ov::pass::Serialize::Version::IR_V11);

    if (version != static_cast<int64_t>(ov::pass::Serialize::Version::IR_V10) &&
        version != static_cast<int64_t>(ov::pass::Serialize::Version::IR_V11)) {
        throw ngraph_error("Unsupported version");
    }
    std::string name = "net";
    pugi::xml_document xml_doc;
    pugi::xml_node net_node = xml_doc.append_child(name.c_str());
    ConstantWriter constant_write_handler(bin_file);
    XmlSerializer visitor(net_node, name, custom_opsets, constant_write_handler, version, deterministic);
    visitor.on_attribute(name, f);

    xml_doc.save(xml_file);
    xml_file.flush();
    bin_file.flush();
};

}  // namespace

namespace ov {
bool pass::Serialize::run_on_function(std::shared_ptr<ngraph::Function> f) {
    if (m_xmlFile && m_binFile) {
        serializeFunc(*m_xmlFile, *m_binFile, f, m_version, m_custom_opsets);
    } else {
        std::ofstream bin_file(m_binPath, std::ios::out | std::ios::binary);
        NGRAPH_CHECK(bin_file, "Can't open bin file: \"" + m_binPath + "\"");

        // create xml file
        std::ofstream xml_file(m_xmlPath, std::ios::out);
        NGRAPH_CHECK(xml_file, "Can't open xml file: \"" + m_xmlPath + "\"");

        try {
            serializeFunc(xml_file, bin_file, f, m_version, m_custom_opsets);
        } catch (const ngraph::CheckFailure&) {
            // optimization decision was made to create .bin file upfront and
            // write to it directly instead of buffering its content in memory,
            // hence we need to delete it here in case of failure
            xml_file.close();
            bin_file.close();
            std::remove(m_xmlPath.c_str());
            std::remove(m_binPath.c_str());
            throw;
        }
    }

    // Return false because we didn't change nGraph Function
    return false;
}

OPENVINO_SUPPRESS_DEPRECATED_START
pass::Serialize::Serialize(std::ostream& xmlFile,
                           std::ostream& binFile,
                           std::map<std::string, ngraph::OpSet> custom_opsets,
                           pass::Serialize::Version version)
    : m_xmlFile{&xmlFile},
      m_binFile{&binFile},
      m_xmlPath{},
      m_binPath{},
      m_version{version},
      m_custom_opsets{custom_opsets} {}

pass::Serialize::Serialize(std::ostream& xmlFile, std::ostream& binFile, pass::Serialize::Version version)
    : pass::Serialize::Serialize(xmlFile, binFile, std::map<std::string, ngraph::OpSet>{}, version) {}

pass::Serialize::Serialize(const std::string& xmlPath,
                           const std::string& binPath,
                           std::map<std::string, ngraph::OpSet> custom_opsets,
                           pass::Serialize::Version version)
    : m_xmlFile{nullptr},
      m_xmlPath{valid_xml_path(xmlPath)},
      m_binPath{provide_bin_path(xmlPath, binPath)},
      m_version{version},
      m_custom_opsets{custom_opsets} {}

pass::Serialize::Serialize(const std::string& xmlPath, const std::string& binPath, pass::Serialize::Version version)
    : pass::Serialize::Serialize(xmlPath, binPath, std::map<std::string, ngraph::OpSet>{}, version) {}
OPENVINO_SUPPRESS_DEPRECATED_END

OPENVINO_SUPPRESS_DEPRECATED_START
pass::StreamSerialize::StreamSerialize(std::ostream& stream,
                                       std::map<std::string, ngraph::OpSet>&& custom_opsets,
                                       const std::function<void(std::ostream&)>& custom_data_serializer,
                                       Serialize::Version version)
    : m_stream(stream),
      m_custom_opsets(std::move(custom_opsets)),
      m_custom_data_serializer(custom_data_serializer),
      m_version(version) {
    if (version != Serialize::Version::UNSPECIFIED && version != Serialize::Version::IR_V10 &&
        version != Serialize::Version::IR_V11) {
        throw ngraph_error("Unsupported version");
    }
}

pass::StreamSerialize::StreamSerialize(std::ostream& stream,
                                       const std::function<void(std::ostream&)>& custom_data_serializer,
                                       Serialize::Version version)
    : StreamSerialize(stream, {}, custom_data_serializer, version) {}
OPENVINO_SUPPRESS_DEPRECATED_END

bool pass::StreamSerialize::run_on_function(std::shared_ptr<ngraph::Function> f) {
    /*
        Format:
        [ DataHeader  ]
        [ Custom data ]
        [    Blobs    ]
        [     IR      ]
    */
    DataHeader hdr = {};

    auto writeHeader = [this](const DataHeader& hdr) {
        m_stream.write((const char*)&hdr, sizeof hdr);
    };
    auto version = static_cast<int64_t>(m_version);
    auto& rt_info = f->get_rt_info();
    if (rt_info.count("version")) {
        auto version_var = std::dynamic_pointer_cast<VariantWrapper<int64_t>>(rt_info.at("version"));
        version = version_var->get();
    }

    if (version != static_cast<int64_t>(m_version) && m_version != Serialize::Version::UNSPECIFIED)
        throw ngraph_error("Cannot serialize function to incompatible IR version");

    if (version == static_cast<int64_t>(Serialize::Version::UNSPECIFIED)) {
        version = static_cast<int64_t>(Serialize::Version::IR_V11);
    }

    // Header
    const size_t header_offset = m_stream.tellp();
    writeHeader(hdr);

    // Custom data
    hdr.custom_data_offset = m_stream.tellp();
    if (m_custom_data_serializer) {
        m_custom_data_serializer(m_stream);
    }

    // Blobs
    hdr.consts_offset = m_stream.tellp();
    std::string name = "net";
    pugi::xml_document xml_doc;
    pugi::xml_node net_node = xml_doc.append_child(name.c_str());
    ConstantWriter constant_write_handler(m_stream);
    XmlSerializer visitor(net_node, name, m_custom_opsets, constant_write_handler, version);
    visitor.on_attribute(name, f);

    // IR
    hdr.model_offset = m_stream.tellp();
    xml_doc.save(m_stream);
    m_stream.flush();

    const size_t file_size = m_stream.tellp();

    hdr.custom_data_size = hdr.consts_offset - hdr.custom_data_offset;
    hdr.consts_size = hdr.model_offset - hdr.consts_offset;
    hdr.model_size = file_size - hdr.model_offset;

    m_stream.seekp(header_offset);
    writeHeader(hdr);

    m_stream.seekp(file_size);

    // Return false because we didn't change nGraph Function
    return false;
}

/// -------- Hash calculation pass -------------

namespace {
template <typename T>
static uint64_t hash_combine(uint64_t seed, const T& a) {
    // Hash combine formula from boost
    return seed ^ (std::hash<T>()(a) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

class OstreamHashWrapper final : public std::streambuf {
    uint64_t m_res = 0;

public:
    uint64_t getResult() const {
        return m_res;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        auto* intS = (const std::streamsize*)s;
        std::streamsize n64 = n / static_cast<std::streamsize>(sizeof(std::streamsize));
        std::streamsize i = 0;
        // Using 64-bit values executes much faster than char
        while (i++ < n64) {
            m_res += *(intS++);
        }

        std::streamsize rest = n % static_cast<std::streamsize>(sizeof(std::streamsize));
        for (i = 0; i < rest; i++) {
            m_res += s[n - rest + i];
        }
        return n;
    }
};
}  // namespace

bool pass::Hash::run_on_function(std::shared_ptr<ov::Function> f) {
    OstreamHashWrapper xmlHash;
    OstreamHashWrapper binHash;
    std::ostream xml(&xmlHash);
    std::ostream bin(&binHash);

    // Determinism is important for hash calculation
    serializeFunc(xml, bin, f, Serialize::Version::UNSPECIFIED, {}, true);

    uint64_t seed = 0;
    seed = hash_combine(seed, xmlHash.getResult());
    seed = hash_combine(seed, binHash.getResult());

    m_hash = seed;
    // Return false because we didn't change nGraph Function
    return false;
}

pass::Hash::Hash(uint64_t& output_hash_value) : m_hash(output_hash_value) {}

}  // namespace ov
