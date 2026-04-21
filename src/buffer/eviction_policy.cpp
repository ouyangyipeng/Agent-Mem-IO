/**
 * @file eviction_policy.cpp
 * @brief Graph-Aware 2Q eviction policy statistics and classification
 *
 * Provides helper functions for the 2Q (hot/cold queue) eviction policy
 * used by BufferPoolManager. Core eviction logic (insert, access, evict)
 * remains in buffer_pool.cpp; this file supplements with:
 *   - In-degree classification thresholds
 *   - Queue distribution statistics
 *   - Eviction policy debug/reasoning helpers
 */

#include "buffer/buffer_pool.h"
#include "common/types.h"
#include <cmath>
#include <algorithm>

namespace agent_mem_io {

// =============================================================================
// Hub Node Classification (Graph-Aware 2Q)
// =============================================================================

std::vector<NodeId> classify_hub_nodes(
    const std::vector<uint32_t>& in_degrees,
    uint32_t hub_threshold
) {
    std::vector<NodeId> hubs;
    for (NodeId i = 0; i < static_cast<NodeId>(in_degrees.size()); ++i) {
        if (in_degrees[i] >= hub_threshold) {
            hubs.push_back(i);
        }
    }
    return hubs;
}

uint32_t compute_hub_threshold(const std::vector<uint32_t>& in_degrees, double percentile) {
    if (in_degrees.empty()) {
        return 1;
    }

    std::vector<uint32_t> sorted = in_degrees;
    std::sort(sorted.begin(), sorted.end());

    // Remove zeros (nodes with no in-degree are never accessed)
    auto non_zero_start = std::upper_bound(sorted.begin(), sorted.end(), 0u);
    if (non_zero_start == sorted.end()) {
        return 1;
    }

    Size non_zero_count = static_cast<Size>(sorted.end() - non_zero_start);
    Size index = static_cast<Size>(std::ceil(non_zero_count * percentile));
    if (index >= non_zero_count) {
        index = non_zero_count - 1;
    }

    Size offset = static_cast<Size>(non_zero_start - sorted.begin()) + index;
    return sorted[offset];
}

// =============================================================================
// Queue Distribution Statistics
// =============================================================================

struct QueueDistributionStats {
    Size hot_queue_size;
    Size cold_queue_size;
    Size free_list_size;
    double hot_ratio;          // hot / (hot + cold + free)
    double effective_hit_rate; // hits / (hits + misses) for recent accesses
};

QueueDistributionStats compute_queue_stats(
    Size hot_size, Size cold_size, Size free_size,
    uint64_t hits, uint64_t misses
) {
    Size total = hot_size + cold_size + free_size;
    QueueDistributionStats stats;
    stats.hot_queue_size = hot_size;
    stats.cold_queue_size = cold_size;
    stats.free_list_size = free_size;
    stats.hot_ratio = (total > 0) ? static_cast<double>(hot_size) / total : 0.0;
    uint64_t total_accesses = hits + misses;
    stats.effective_hit_rate = (total_accesses > 0)
        ? static_cast<double>(hits) / total_accesses : 0.0;
    return stats;
}

}  // namespace agent_mem_io