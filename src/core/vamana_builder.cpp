/**
 * @file vamana_builder.cpp
 * @brief Vamana graph builder standalone utilities
 *
 * Provides helper functions for Vamana/DiskANN graph construction:
 *   - Random graph initialization
 *   - Medoid (entry point) selection
 *   - RobustPrune neighbor selection
 *
 * Core VamanaBuilder::build() and add_node_incremental() remain in
 * graph_index.cpp since they operate on GraphNavData directly.
 */

#include "core/graph_index.h"
#include "core/simd_distance.h"
#include "common/types.h"
#include <random>
#include <algorithm>
#include <limits>
#include <queue>

namespace agent_mem_io {

// =============================================================================
// Medoid Selection (Entry Point for Vamana/DiskANN)
// =============================================================================

NodeId find_medoid(const std::vector<Vector>& vectors) {
    if (vectors.empty()) {
        return INVALID_NODE_ID;
    }

    Size dim = vectors[0].size();
    Size num_nodes = vectors.size();

    // Compute centroid
    Vector centroid(dim, 0.0f);
    for (const auto& vec : vectors) {
        for (Size i = 0; i < dim; ++i) {
            centroid[i] += vec[i];
        }
    }
    for (Size i = 0; i < dim; ++i) {
        centroid[i] /= static_cast<float>(num_nodes);
    }

    // Find closest vector to centroid (the medoid)
    Distance min_dist = std::numeric_limits<Distance>::max();
    NodeId medoid = 0;
    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        Distance dist = l2_distance_sq_simd(centroid.data(), vectors[i].data(), dim);
        if (dist < min_dist) {
            min_dist = dist;
            medoid = i;
        }
    }

    return medoid;
}

// =============================================================================
// Random Graph Initialization
// =============================================================================

void init_random_graph(
    const std::vector<Vector>& vectors,
    Size max_degree,
    GraphNavData& nav_data
) {
    Size num_nodes = vectors.size();
    if (num_nodes == 0) return;

    std::random_device rd;
    std::mt19937 rng(rd());

    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        auto& neighbors = nav_data.get_neighbors_mut(i);
        neighbors.clear();

        // Select max_degree random neighbors (excluding self)
        std::uniform_int_distribution<NodeId> dist(0, static_cast<NodeId>(num_nodes - 1));
        std::unordered_set<NodeId> selected;
        selected.reserve(max_degree);

        while (selected.size() < max_degree && selected.size() < num_nodes - 1) {
            NodeId candidate = dist(rng);
            if (candidate != i && selected.find(candidate) == selected.end()) {
                selected.insert(candidate);
            }
        }

        for (NodeId n : selected) {
            neighbors.push_back(n);
        }
    }
}

// =============================================================================
// RobustPrune — DiskANN Neighbor Selection Algorithm
// =============================================================================

std::vector<NodeId> robust_prune(
    NodeId node_id,
    const std::vector<std::pair<Distance, NodeId>>& candidates,
    const std::vector<Vector>& vectors,
    Size max_degree,
    float alpha
) {
    if (candidates.empty()) {
        return {};
    }

    // Sort candidates by distance (closest first)
    std::vector<std::pair<Distance, NodeId>> sorted = candidates;
    std::sort(sorted.begin(), sorted.end());

    std::vector<NodeId> result;
    result.reserve(max_degree);

    const Vector& node_vec = vectors[node_id];

    for (const auto& [dist, candidate_id] : sorted) {
        if (result.size() >= max_degree) break;

        // Check if candidate is "alpha-close" to any already-selected neighbor
        // If dist(candidate, node) < alpha * dist(candidate, any_selected_neighbor),
        // then candidate is redundant (too similar to an existing neighbor)
        bool is_redundant = false;
        for (NodeId selected_id : result) {
            Distance candidate_to_selected = l2_distance_sq_simd(
                vectors[candidate_id].data(), vectors[selected_id].data(),
                node_vec.size());
            if (alpha * candidate_to_selected < dist) {
                is_redundant = true;
                break;
            }
        }

        if (!is_redundant) {
            result.push_back(candidate_id);
        }
    }

    return result;
}

}  // namespace agent_mem_io