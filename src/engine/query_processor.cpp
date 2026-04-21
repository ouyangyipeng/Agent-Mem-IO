/**
 * @file query_processor.cpp
 * @brief Query processor standalone search with LSM-Tree integration
 *
 * Provides an enhanced search function that combines graph-based ANN search
 * with LSM-Tree write path lookups, ensuring recently-written vectors
 * (still in MemTable or SSTable) are discoverable during search.
 * Core QueryProcessor implementation remains in storage_engine.cpp.
 */

#include "engine/storage_engine.h"
#include "io/disk_layout.h"
#include "core/simd_distance.h"
#include "core/visited_bitmap.h"
#include <algorithm>
#include <queue>
#include <unordered_set>

namespace agent_mem_io {

// =============================================================================
// Enhanced Search with LSM-Tree Read-back
// =============================================================================

std::vector<NodeId> search_with_lsm_fallback(
    const Vector& query,
    Size k,
    Size ef_search,
    Size beam_width,
    const GraphIndex& graph_index,
    const std::vector<Vector>& base_vectors,
    const LsmWriteManager* write_manager,
    DiskIndexReader* disk_reader = nullptr
) {
    Size num_base = base_vectors.size();

    // Standard graph-based search on base dataset
    VisitedBitmap visited(num_base + (write_manager ? write_manager->get_total_entries() : 0));
    visited.clear();

    // Start from graph entry point
    NodeId entry = graph_index.get_entry_point();
    if (entry == INVALID_NODE_ID || num_base == 0) {
        return {};
    }

    // Priority queue: (distance, node_id) — min-heap for candidates
    using Neighbor = std::pair<float, NodeId>;
    std::priority_queue<Neighbor, std::vector<Neighbor>, std::greater<Neighbor>> candidates;
    std::priority_queue<Neighbor, std::vector<Neighbor>, std::less<Neighbor>> result_heap;

    // Initialize with entry point from base dataset
    Distance entry_dist = l2_distance_sq_simd(
        query.data(), base_vectors[entry].data(), query.size());
    candidates.emplace(entry_dist, entry);
    result_heap.emplace(entry_dist, entry);
    visited.set(entry);

    Size search_list_size = ef_search;
    while (!candidates.empty()) {
        auto [current_dist, current_id] = candidates.top();
        candidates.pop();

        // Early termination: if worst result is closer than best remaining candidate
        if (result_heap.size() >= k && current_dist > result_heap.top().first) {
            break;
        }

        // Get neighbors from graph (base dataset nodes)
        const auto& neighbors = graph_index.get_neighbors(current_id);
        for (NodeId neighbor : neighbors) {
            if (neighbor >= num_base) continue;  // Skip out-of-range IDs
            if (!visited.test(neighbor)) {
                visited.set(neighbor);

                Vector neighbor_vec;
                if (disk_reader && disk_reader->is_open()) {
                    neighbor_vec = disk_reader->read_vector(neighbor);
                } else if (neighbor < base_vectors.size()) {
                    neighbor_vec = base_vectors[neighbor];
                }

                if (neighbor_vec.size() != query.size()) continue;

                Distance d = l2_distance_sq_simd(
                    query.data(), neighbor_vec.data(), query.size());

                if (result_heap.size() < k || d < result_heap.top().first) {
                    candidates.emplace(d, neighbor);
                    if (result_heap.size() < k) {
                        result_heap.emplace(d, neighbor);
                    } else {
                        result_heap.pop();
                        result_heap.emplace(d, neighbor);
                    }
                }

                // Bound candidate list size
                if (candidates.size() > search_list_size) {
                    std::vector<Neighbor> temp;
                    while (!candidates.empty() && temp.size() < search_list_size) {
                        temp.push_back(candidates.top());
                        candidates.pop();
                    }
                    candidates = std::priority_queue<Neighbor,
                        std::vector<Neighbor>, std::greater<Neighbor>>(
                            std::greater<Neighbor>(), std::move(temp));
                }
            }
        }
    }

    // Collect results from heap
    std::vector<NodeId> results;
    while (!result_heap.empty()) {
        results.push_back(result_heap.top().second);
        result_heap.pop();
    }
    std::reverse(results.begin(), results.end());

    // LSM-Tree fallback: check recently-written vectors
    // For each vector still in the write path, compute distance and
    // merge into results if closer than worst result
    if (write_manager && write_manager->get_total_entries() > 0) {
        Size total_lsm_entries = write_manager->get_total_entries();
        for (NodeId lsm_id = 0; lsm_id < static_cast<NodeId>(total_lsm_entries); ++lsm_id) {
            Vector lsm_vec;
            if (write_manager->get(lsm_id, lsm_vec)) {
                if (lsm_vec.size() != query.size()) continue;

                Distance d = l2_distance_sq_simd(
                    query.data(), lsm_vec.data(), query.size());

                // Check if this LSM vector is closer than worst result
                if (results.size() < k || d < l2_distance_sq_simd(
                    query.data(), base_vectors[results.back()].data(), query.size())) {
                    // Insert into results maintaining sorted order
                    auto it = std::lower_bound(results.begin(), results.end(), d,
                        [&](NodeId id, float dist) {
                            float id_dist = l2_distance_sq_simd(
                                query.data(), base_vectors[id].data(), query.size());
                            return id_dist < dist;
                        });
                    results.insert(it, lsm_id);
                    if (results.size() > k) {
                        results.pop_back();
                    }
                }
            }
        }
    }

    // Trim to k results
    if (results.size() > k) {
        results.resize(k);
    }

    return results;
}

}  // namespace agent_mem_io