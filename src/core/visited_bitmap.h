/**
 * @file visited_bitmap.h
 * @brief Fast bitmap-based visited set for graph search
 *
 * Replaces unordered_set<NodeId> with a fixed-size bitmap.
 * For 1M nodes: bitmap size = 1M/8 = 128KB (vs unordered_set ~40MB+)
 * Lookup/set is O(1) with simple array access, no hash computation.
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <cstdint>
#include <cstring>

namespace agent_mem_io {

class VisitedBitmap {
public:
    /**
     * @brief Construct bitmap for max_num_nodes nodes
     * @param max_num_nodes Maximum number of nodes to track
     */
    explicit VisitedBitmap(Size max_num_nodes)
        : size_(max_num_nodes)
        , bitmap_((max_num_nodes + 7) / 8, 0) {
    }

    /**
     * @brief Test if a node has been visited
     * @param id Node ID
     * @return true if visited
     */
    bool test(NodeId id) const {
        if (id >= size_) return true;  // Out-of-range treated as visited (skip)
        return bitmap_[id >> 3] & (1 << (id & 7));
    }

    /**
     * @brief Mark a node as visited
     * @param id Node ID
     */
    void set(NodeId id) {
        if (id >= size_) return;
        bitmap_[id >> 3] |= (1 << (id & 7));
    }

    /**
     * @brief Clear all visited marks (for reuse in next search)
     */
    void clear() {
        std::memset(bitmap_.data(), 0, bitmap_.size());
    }

    /**
     * @brief Get memory usage
     * @return Memory usage in bytes
     */
    Size memory_usage() const {
        return bitmap_.size();
    }

private:
    Size size_;
    std::vector<uint8_t> bitmap_;
};

}  // namespace agent_mem_io