/**
 * @file vector_dataset.cpp
 * @brief Vector dataset utilities — normalization, statistics, and validation
 *
 * Provides utility functions for vector dataset preprocessing:
 *   - L2 normalization
 *   - Dataset statistics (dimension distribution, mean/std)
 *   - Data validation (dimension consistency, value range checks)
 *
 * Actual dataset loading (SIFT1M, synthetic) remains in sift_loader.h
 * and synthetic_data.h respectively.
 */

#include "common/types.h"
#include "core/simd_distance.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace agent_mem_io {

// =============================================================================
// Vector Normalization
// =============================================================================

void normalize_vector_l2(Vector& vec) {
    float norm_sq = 0.0f;
    for (float v : vec) {
        norm_sq += v * v;
    }
    float norm = std::sqrt(norm_sq);
    if (norm > 1e-8f) {
        for (float& v : vec) {
            v /= norm;
        }
    }
    // Zero vectors remain zero (avoid division by near-zero)
}

void normalize_dataset_l2(std::vector<Vector>& dataset) {
    for (auto& vec : dataset) {
        normalize_vector_l2(vec);
    }
}

// =============================================================================
// Dataset Statistics
// =============================================================================

struct DatasetStats {
    Size num_vectors;
    Dimension dimension;
    float mean_norm;         // Average L2 norm
    float min_norm;          // Minimum L2 norm
    float max_norm;          // Maximum L2 norm
    float std_norm;          // Standard deviation of norms
    float mean_value;        // Average element value
    float min_value;         // Minimum element value
    float max_value;         // Maximum element value
};

DatasetStats compute_dataset_stats(const std::vector<Vector>& dataset) {
    if (dataset.empty()) {
        DatasetStats empty_stats;
        empty_stats.num_vectors = 0;
        empty_stats.dimension = 0;
        empty_stats.mean_norm = 0.0f;
        empty_stats.min_norm = 0.0f;
        empty_stats.max_norm = 0.0f;
        empty_stats.std_norm = 0.0f;
        empty_stats.mean_value = 0.0f;
        empty_stats.min_value = 0.0f;
        empty_stats.max_value = 0.0f;
        return empty_stats;
    }

    DatasetStats stats;
    stats.num_vectors = dataset.size();
    stats.dimension = dataset[0].size();

    // Compute norms
    std::vector<float> norms;
    norms.reserve(dataset.size());

    float total_value = 0.0f;
    float global_min = std::numeric_limits<float>::max();
    float global_max = std::numeric_limits<float>::lowest();
    Size total_elements = 0;

    for (const auto& vec : dataset) {
        float norm_sq = 0.0f;
        for (float v : vec) {
            norm_sq += v * v;
            total_value += v;
            global_min = std::min(global_min, v);
            global_max = std::max(global_max, v);
            total_elements++;
        }
        norms.push_back(std::sqrt(norm_sq));
    }

    // Compute norm statistics
    float sum_norms = std::accumulate(norms.begin(), norms.end(), 0.0f);
    stats.mean_norm = sum_norms / static_cast<float>(norms.size());
    stats.min_norm = *std::min_element(norms.begin(), norms.end());
    stats.max_norm = *std::max_element(norms.begin(), norms.end());

    float sum_sq_diff = 0.0f;
    for (float n : norms) {
        float diff = n - stats.mean_norm;
        sum_sq_diff += diff * diff;
    }
    stats.std_norm = std::sqrt(sum_sq_diff / static_cast<float>(norms.size()));

    // Value statistics
    stats.mean_value = (total_elements > 0) ? total_value / static_cast<float>(total_elements) : 0.0f;
    stats.min_value = global_min;
    stats.max_value = global_max;

    return stats;
}

// =============================================================================
// Dataset Validation
// // =============================================================================

bool validate_dataset(const std::vector<Vector>& dataset) {
    if (dataset.empty()) {
        return true;  // Empty dataset is valid (but useless)
    }

    Dimension expected_dim = dataset[0].size();
    if (expected_dim == 0) {
        return false;  // Zero-dimensional vectors are invalid
    }

    // Check dimension consistency
    for (Size i = 1; i < dataset.size(); ++i) {
        if (dataset[i].size() != expected_dim) {
            return false;
        }
    }

    // Check for NaN/Inf values
    for (const auto& vec : dataset) {
        for (float v : vec) {
            if (std::isnan(v) || std::isinf(v)) {
                return false;
            }
        }
    }

    return true;
}

}  // namespace agent_mem_io