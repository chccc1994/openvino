// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "openvino/core/coordinate_diff.hpp"  // ov::CoordinateDiff

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <iterator>
#include <sstream>
#include <string>

#include "pyopenvino/graph/coordinate_diff.hpp"

namespace py = pybind11;

void regclass_graph_CoordinateDiff(py::module m) {
    py::class_<ov::CoordinateDiff, std::shared_ptr<ov::CoordinateDiff>> coordinate_diff(m, "CoordinateDiff");
    coordinate_diff.doc() = "openvino.impl.CoordinateDiff wraps ov::CoordinateDiff";
    coordinate_diff.def(py::init<const std::initializer_list<ptrdiff_t>&>());
    coordinate_diff.def(py::init<const std::vector<ptrdiff_t>&>());
    coordinate_diff.def(py::init<const ov::CoordinateDiff&>());

    coordinate_diff.def("__str__", [](const ov::CoordinateDiff& self) -> std::string {
        std::stringstream stringstream;
        std::copy(self.begin(), self.end(), std::ostream_iterator<int>(stringstream, ", "));
        std::string string = stringstream.str();
        return string.substr(0, string.size() - 2);
    });

    coordinate_diff.def("__repr__", [](const ov::CoordinateDiff& self) -> std::string {
        std::string class_name = py::cast(self).get_type().attr("__name__").cast<std::string>();
        std::string shape_str = py::cast(self).attr("__str__")().cast<std::string>();
        return "<" + class_name + ": (" + shape_str + ")>";
    });
}
