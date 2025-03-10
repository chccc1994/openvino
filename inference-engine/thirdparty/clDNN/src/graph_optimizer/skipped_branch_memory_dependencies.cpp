// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

///////////////////////////////////////////////////////////////////////////////////////////////////

#include "pass_manager.h"
#include "program_node.h"
#include "layout_optimizer.h"
#include "cldnn/graph/program.hpp"
#include "program_helpers.h"
#include "runtime/cldnn_itt.hpp"
#include <vector>
#include <memory>
#include <list>
#include <map>
#include <set>

using namespace cldnn;

void skipped_branch_memory_dependencies::run(program& p) {
    OV_ITT_SCOPED_TASK(itt::domains::CLDNN, "CLDNN::pass::SkippedBranchMemoryDependencies");
    // Primitive A can't use primitive B buffer if processing_num(B) < processing_num(A) and for any usr - the user of B
    // processing_num(usr) > processing_num(A) Otherwise it could override data that has to be used in the future.
    auto& processing_order = p.get_processing_order();
    auto itrB = processing_order.begin();
    while (itrB != processing_order.end()) {
        auto& nodeB = *itrB;
        auto itrA = ++itrB;
        if (nodeB->get_users().size() == 0)
            continue;

        // find the last user of B in processing order
        auto itrUsr = nodeB->get_users().begin();
        auto lastUsr = itrUsr++;
        while (itrUsr != nodeB->get_users().end()) {
            if (processing_order.get_processing_number(*lastUsr) < processing_order.get_processing_number(*itrUsr))
                lastUsr = itrUsr;
            itrUsr++;
        }

        // mark all nodes in between B and lastUsr of B as forbidden to share buffer with B
        while (itrA != processing_order.get_processing_iterator(**lastUsr) && itrA != processing_order.end()) {
            auto& nodeA = *itrA;
            itrA++;
            add_memory_dependency(nodeA, nodeB);
            add_memory_dependency(nodeB, nodeA);
        }
    }
}
