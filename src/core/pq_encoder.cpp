/**
 * @file pq_encoder.cpp
 * @brief Product Quantization encoder implementation
 *
 * Implements PQ training (k-means per subspace), encoding, decoding,
 * and ADC distance table computation following the DiskANN approach.
 */

#include "core/pq_encoder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <cassert>
#include <iostream>

namespace agent_mem_io {

// =============================================================================
// PQEncoder Implementation
// =============================================================================

PQEncoder::PQEncoder(Dimension dim, Size m, Size k)
    : dim_(dim), m_(m), k_(k), trained_(false) {
    // Ensure dimension is divisible by m
    assert(dim % m == 0 && "Dimension must be divisible by number of subspaces");
    sub_dim_ = dim / m;

    // Initialize codebooks storage (will be populated during training)
    codebooks_.resize(m_);
    for (Size j = 0; j < m_; ++j) {
        codebooks_[j].resize(k_);
        for (Size i = 0; i < k_; ++i) {
            codebooks_[j][i].resize(sub_dim_, 0.0f);
        }
    }
}

bool PQEncoder::train(const std::vector<Vector>& training_data,
                       Size num_iterations) {
    if (training_data.empty()) {
        std::cerr << "[PQEncoder] Error: empty training data\n";
        return false;
    }

    // Use a sample if training data is too large (k-means is expensive)
    Size sample_size = training_data.size();
    if (sample_size > 100000) {
        sample_size = 100000;  // Limit training sample for efficiency
    }

    // Create a random sample
    std::mt19937 rng(42);
    std::vector<Size> indices(training_data.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::shuffle(indices.begin(), indices.end(), rng);
    if (indices.size() > sample_size) {
        indices.resize(sample_size);
    }

    std::cout << "[PQEncoder] Training with " << indices.size()
              << " samples, " << num_iterations << " iterations\n";

    // Train each subspace independently
    for (Size j = 0; j < m_; ++j) {
        // Extract subvectors for this subspace
        std::vector<Vector> subvectors;
        subvectors.reserve(indices.size());
        for (Size idx : indices) {
            subvectors.push_back(extract_subvector(training_data[idx], j));
        }

        // Run k-means on subvectors
        codebooks_[j] = kmeans(subvectors, num_iterations);
    }

    trained_ = true;
    std::cout << "[PQEncoder] Training complete. Codebook size: "
              << calculate_codebook_memory() / 1024.0 << " KB\n";
    return true;
}

PQCodeVector PQEncoder::encode(const Vector& vector) const {
    assert(trained_ && "PQ encoder must be trained before encoding");
    assert(vector.size() == dim_ && "Vector dimension mismatch");

    PQCodeVector codes(m_);
    for (Size j = 0; j < m_; ++j) {
        Vector sub = extract_subvector(vector, j);
        codes[j] = find_nearest_centroid(sub, j);
    }
    return codes;
}

std::vector<PQCodeVector> PQEncoder::encode_batch(const std::vector<Vector>& vectors) const {
    assert(trained_ && "PQ encoder must be trained before encoding");

    std::vector<PQCodeVector> all_codes(vectors.size());
    for (Size i = 0; i < vectors.size(); ++i) {
        all_codes[i] = encode(vectors[i]);
    }
    return all_codes;
}

Vector PQEncoder::decode(const PQCodeVector& codes) const {
    assert(trained_ && "PQ encoder must be trained before decoding");
    assert(codes.size() == m_ && "PQ code vector size mismatch");

    Vector reconstructed(dim_, 0.0f);
    for (Size j = 0; j < m_; ++j) {
        // Copy centroid values back to the full vector position
        const Vector& centroid = codebooks_[j][codes[j]];
        for (Dimension d = 0; d < sub_dim_; ++d) {
            reconstructed[j * sub_dim_ + d] = centroid[d];
        }
    }
    return reconstructed;
}

Size PQEncoder::calculate_pq_memory(Size num_vectors) const {
    // m bytes per vector (PQ codes)
    return num_vectors * m_;
}

Size PQEncoder::calculate_codebook_memory() const {
    // m × k × sub_dim × sizeof(float)
    return m_ * k_ * sub_dim_ * sizeof(float);
}

// =============================================================================
// Private Methods
// =============================================================================

Vector PQEncoder::extract_subvector(const Vector& vector, Size subspace_idx) const {
    Vector sub(sub_dim_);
    Size offset = subspace_idx * sub_dim_;
    for (Dimension d = 0; d < sub_dim_; ++d) {
        sub[d] = vector[offset + d];
    }
    return sub;
}

PQCode PQEncoder::find_nearest_centroid(const Vector& subvector, Size subspace_idx) const {
    float min_dist = std::numeric_limits<float>::max();
    PQCode best = 0;

    for (Size i = 0; i < k_; ++i) {
        float dist = l2_distance_sq(subvector, codebooks_[subspace_idx][i]);
        if (dist < min_dist) {
            min_dist = dist;
            best = static_cast<PQCode>(i);
        }
    }
    return best;
}

float PQEncoder::l2_distance_sq(const Vector& a, const Vector& b) {
    float sum = 0.0f;
    for (Size i = 0; i < a.size() && i < b.size(); ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

std::vector<Vector> PQEncoder::kmeans(const std::vector<Vector>& subvectors,
                                       Size num_iterations) const {
    assert(subvectors.size() >= k_ && "Need at least k subvectors for k-means");

    std::mt19937 rng(42);

    // Initialize centroids: randomly select k subvectors as initial centroids
    std::vector<Size> init_indices(subvectors.size());
    std::iota(init_indices.begin(), init_indices.end(), 0);
    std::shuffle(init_indices.begin(), init_indices.end(), rng);

    std::vector<Vector> centroids(k_);
    for (Size i = 0; i < k_; ++i) {
        centroids[i] = subvectors[init_indices[i]];
    }

    // Assignment array: which centroid each subvector belongs to
    std::vector<Size> assignments(subvectors.size(), 0);

    // K-means iterations
    for (Size iter = 0; iter < num_iterations; ++iter) {
        // Assignment step: assign each subvector to nearest centroid
        bool changed = false;
        for (Size v = 0; v < subvectors.size(); ++v) {
            float min_dist = std::numeric_limits<float>::max();
            Size best = 0;
            for (Size c = 0; c < k_; ++c) {
                float dist = l2_distance_sq(subvectors[v], centroids[c]);
                if (dist < min_dist) {
                    min_dist = dist;
                    best = c;
                }
            }
            if (assignments[v] != best) {
                changed = true;
                assignments[v] = best;
            }
        }

        // Early termination if no assignments changed
        if (!changed && iter > 0) {
            std::cout << "[PQEncoder] K-means converged at iteration " << iter << "\n";
            break;
        }

        // Update step: recompute centroids as mean of assigned subvectors
        std::vector<Size> counts(k_, 0);
        std::vector<Vector> new_centroids(k_, Vector(sub_dim_, 0.0f));

        for (Size v = 0; v < subvectors.size(); ++v) {
            Size c = assignments[v];
            counts[c]++;
            for (Dimension d = 0; d < sub_dim_; ++d) {
                new_centroids[c][d] += subvectors[v][d];
            }
        }

        for (Size c = 0; c < k_; ++c) {
            if (counts[c] > 0) {
                for (Dimension d = 0; d < sub_dim_; ++d) {
                    new_centroids[c][d] /= static_cast<float>(counts[c]);
                }
                centroids[c] = new_centroids[c];
            }
            // If a centroid has no assignments, keep it unchanged (or reinit)
        }
    }

    return centroids;
}

// =============================================================================
// PQDistanceTable Implementation
// =============================================================================

PQDistanceTable::PQDistanceTable(const PQEncoder& encoder)
    : encoder_(encoder), built_(false) {
    // Pre-allocate table: m × k floats
    table_.resize(encoder_.get_m());
    for (Size j = 0; j < encoder_.get_m(); ++j) {
        table_[j].resize(encoder_.get_k(), 0.0f);
    }
}

void PQDistanceTable::build(const Vector& query) {
    assert(query.size() == encoder_.get_dimension() && "Query dimension mismatch");
    assert(encoder_.is_trained() && "PQ encoder must be trained");

    Dimension sub_dim = encoder_.get_sub_dim();
    Size m = encoder_.get_m();
    Size k = encoder_.get_k();
    const auto& codebooks = encoder_.get_codebooks();

    // For each subspace, compute distance from query subvector to all centroids
    for (Size j = 0; j < m; ++j) {
        // Extract query subvector
        Size offset = j * sub_dim;

        // Compute ||query_sub_j - centroid_j_i||^2 for all i
        for (Size i = 0; i < k; ++i) {
            float dist_sq = 0.0f;
            for (Dimension d = 0; d < sub_dim; ++d) {
                float diff = query[offset + d] - codebooks[j][i][d];
                dist_sq += diff * diff;
            }
            table_[j][i] = dist_sq;
        }
    }

    built_ = true;
}

float PQDistanceTable::compute_distance(const PQCodeVector& codes) const {
    assert(built_ && "Distance table must be built before computing distances");
    assert(codes.size() == encoder_.get_m() && "PQ code size mismatch");

    float total_dist_sq = 0.0f;
    for (Size j = 0; j < encoder_.get_m(); ++j) {
        total_dist_sq += table_[j][codes[j]];
    }
    return total_dist_sq;  // Returns L2 squared distance approximation
}

float PQDistanceTable::compute_partial_distance(Size subspace_idx, PQCode code) const {
    assert(built_ && "Distance table must be built");
    assert(subspace_idx < encoder_.get_m() && "Subspace index out of range");
    return table_[subspace_idx][code];
}

Size PQDistanceTable::get_table_size() const {
    return encoder_.get_m() * encoder_.get_k() * sizeof(float);
}

}  // namespace agent_mem_io