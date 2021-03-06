/*
 * Copyright (C) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/command_stream/command_stream_receiver.h"
#include "runtime/command_stream/linear_stream.h"
#include "runtime/event/event.h"
#include "runtime/helpers/kernel_commands.h"
#include "runtime/helpers/timestamp_packet.h"
#include "runtime/utilities/tag_allocator.h"

using namespace OCLRT;

void TimestampPacketContainer::add(Node *timestampPacketNode) {
    timestampPacketNodes.push_back(timestampPacketNode);
}

TimestampPacketContainer::~TimestampPacketContainer() {
    for (auto node : timestampPacketNodes) {
        node->returnTag();
    }
}

void TimestampPacketContainer::swapNodes(TimestampPacketContainer &timestampPacketContainer) {
    timestampPacketNodes.swap(timestampPacketContainer.timestampPacketNodes);
}

void TimestampPacketContainer::resolveDependencies(bool clearAllDependencies) {
    std::vector<Node *> pendingNodes;

    for (auto node : timestampPacketNodes) {
        if (node->tag->canBeReleased() || clearAllDependencies) {
            node->returnTag();
        } else {
            pendingNodes.push_back(node);
        }
    }

    std::swap(timestampPacketNodes, pendingNodes);
}

void TimestampPacketContainer::assignAndIncrementNodesRefCounts(const TimestampPacketContainer &inputTimestampPacketContainer) {
    auto &inputNodes = inputTimestampPacketContainer.peekNodes();
    std::copy(inputNodes.begin(), inputNodes.end(), std::back_inserter(timestampPacketNodes));

    for (auto node : inputNodes) {
        node->incRefCount();
    }
}

void TimestampPacketContainer::makeResident(CommandStreamReceiver &commandStreamReceiver) {
    for (auto node : timestampPacketNodes) {
        commandStreamReceiver.makeResident(*node->getGraphicsAllocation());
    }
}
