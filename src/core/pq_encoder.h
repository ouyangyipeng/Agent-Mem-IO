/**
 * @file pq_encoder.h
 * @brief Product Quantization (PQ) encoder for vector compression
 *
 * Implements the PQ algorithm from Jegou et al. (2011) "Product Quantization
 * for Nearest Neighbor Search". Used by DiskANN to compress vectors for
 * in-memory graph traversal, reducing memory from 512 bytes/vector to 8 bytes.
 *
 * Key idea: Split each d-dimensional vector into m subvectors, quantize each
 * independently using k-means with k=256 centroids. This gives 64x compression
 * for SIFT1M (128-dim, m=8, k=256).
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <cstdint>
#include <random>
#include <cmath>
#include <algorithm>
#include <cassert>

namespace agent_mem_io {

// =============================================================================
// PQ Configuration
// =============================================================================

/// Default number of PQ subspaces (m=8 for 128-dim vectors)
constexpr Size DEFAULT_PQ_M = 8;

/// Default number of centroids per subspace (k=256 = 1 byte per code)
constexpr Size DEFAULT_PQ_K = 256;

/// Default number of k-means iterations for codebook training
constexpr Size DEFAULT_PQ_TRAIN_ITERATIONS = 25;

/// PQ code type (1 byte per subspace, k=256)
using PQCode = uint8_t;

/// PQ code vector type (m bytes per vector)
using PQCodeVector = std::vector<PQCode>;

// =============================================================================
// PQ Encoder
// =============================================================================

/**
 * @brief Product Quantization encoder
 *
 * Trains PQ codebooks using k-means on subvectors, then encodes
 * full-precision vectors into compact PQ codes (8 bytes for SIFT1M).
 *
 * Memory layout:
 * - Codebooks: m × k × sub_dim floats (8 × 256 × 16 × 4B = 128KB)
 * - PQ codes: m bytes per vector (8 bytes for 128-dim)
 * - Total for SIFT1M: 128KB + 1M × 8B = ~8.1MB
 */
class PQEncoder {
public:
    /**
     * @brief Construct PQ encoder
     * @param dim Full vector dimension (e.g., 128 for SIFT)
     * @param m Number of subspaces (e.g., 8)
     * @param k Number of centroids per subspace (e.g., 256)
     */
    PQEncoder(Dimension dim = DEFAULT_DIMENSION,
              Size m = DEFAULT_PQ_M,
              Size k = DEFAULT_PQ_K);

    /**
     * @brief Train PQ codebooks from training data
     * @param training_data Vector dataset for training (sample if too large)
     * @param num_iterations Number of k-means iterations
     * @return true if training succeeded
     */
    bool train(const std::vector<Vector>& training_data,
               Size num_iterations = DEFAULT_PQ_TRAIN_ITERATIONS);

    /**
     * @brief Encode a single vector into PQ codes
     * @param vector Full-precision vector to encode
     * @return PQ code vector (m bytes)
     */
    PQCodeVector encode(const Vector& vector) const;

    /**
     * @brief Encode all vectors in a dataset
     * @param vectors Full-precision vector dataset
     * @return Vector of PQ code vectors
     */
    std::vector<PQCodeVector> encode_batch(const std::vector<Vector>& vectors) const;

    /**
     * @brief Decode a PQ code vector back to approximate full-precision vector
     * @param codes PQ code vector
     * @return Approximate reconstructed vector
     */
    Vector decode(const PQCodeVector& codes) const;

    /**
     * @brief Get the codebooks
     * @return Reference to codebooks (m × k × sub_dim)
     */
    const std::vector<std::vector<Vector>>& get_codebooks() const { return codebooks_; }

    /**
     * @brief Get PQ parameters
     */
    Dimension get_dimension() const { return dim_; }
    Size get_m() const { return m_; }
    Size get_k() const { return k_; }
    Dimension get_sub_dim() const { return sub_dim_; }

    /**
     * @brief Calculate memory usage for PQ codes
     * @param num_vectors Number of vectors
     * @return Memory usage in bytes
     */
    Size calculate_pq_memory(Size num_vectors) const;

    /**
     * @brief Calculate codebook memory usage
     * @return Memory usage in bytes
     */
    Size calculate_codebook_memory() const;

    /**
     * @brief Check if codebooks have been trained
     * @return true if trained
     */
    bool is_trained() const { return trained_; }

private:
    Dimension dim_;        // Full vector dimension
    Size m_;               // Number of subspaces
    Size k_;               // Number of centroids per subspace
    Dimension sub_dim_;    // Subvector dimension (dim / m)
    bool trained_;         // Whether codebooks have been trained

    /// Codebooks: codebooks_[subspace_idx][centroid_idx] = centroid_vector (sub_dim floats)
    std::vector<std::vector<Vector>> codebooks_;

    /**
     * @brief Extract subvector from a full vector
     * @param vector Full vector
     * @param subspace_idx Subspace index (0 to m-1)
     * @return Subvector of sub_dim_ elements
     */
    Vector extract_subvector(const Vector& vector, Size subspace_idx) const;

    /**
     * @brief Find nearest centroid for a subvector
     * @param subvector Subvector to quantize
     * @param subspace_idx Which subspace's codebook to use
     * @return Index of nearest centroid (0 to k-1)
     */
    PQCode find_nearest_centroid(const Vector& subvector, Size subspace_idx) const;

    /**
     * @brief Compute L2 squared distance between two vectors
     * @param a First vector
     * @param b Second vector
     * @return L2 squared distance
     */
    static float l2_distance_sq(const Vector& a, const Vector& b);

    /**
     * @brief Run k-means on a set of subvectors to produce a codebook
     * @param subvectors Training subvectors for one subspace
     * @param num_iterations Number of k-means iterations
     * @return Learned codebook (k centroids, each sub_dim floats)
     */
    std::vector<Vector> kmeans(const std::vector<Vector>& subvectors,
                                Size num_iterations) const;
};

// =============================================================================
// PQ Distance Table (ADC - Asymmetric Distance Computation)
// =============================================================================

/**
 * @brief Precomputed distance lookup table for PQ ADC search
 *
 * This is the key efficiency mechanism: for each query, we precompute
 * a distance table of size m × k (8 × 256 = 2048 floats = 8KB).
 * The table fits in L1 cache, making ADC distance computation extremely fast:
 * just m table lookups and additions per vector.
 *
 * Usage:
 *   PQDistanceTable table(encoder);
 *   table.build(query_vector);
 *   float dist = table.compute_distance(pq_code);  // O(m) lookup+add
 */
class PQDistanceTable {
public:
    /**
     * @brief Construct distance table for a given PQ encoder
     * @param encoder PQ encoder (must be trained)
     */
    explicit PQDistanceTable(const PQEncoder& encoder);

    /**
     * @brief Build distance lookup table for a query vector
     * @param query Query vector (full precision, NOT compressed)
     *
     * For each subspace j and each centroid c_j_i:
     *   table_[j][i] = ||query_sub_j - c_j_i||^2
     *
     * Table size: m × k floats = 8 × 256 × 4B = 8KB
     */
    void build(const Vector& query);

    /**
     * @brief Compute approximate L2 squared distance using ADC
     * @param codes PQ code vector of a database entry
     * @return Approximate L2 squared distance to the query
     *
     * dist_sq ≈ Σ_j table_[j][codes[j]]  (m lookups + m additions)
     */
    float compute_distance(const PQCodeVector& codes) const;

    /**
     * @brief Compute approximate L2 squared distance for a single subspace
     * @param subspace_idx Subspace index
     * @param code PQ code for that subspace
     * @return Partial distance contribution
     */
    float compute_partial_distance(Size subspace_idx, PQCode code) const;

    /**
     * @brief Get the raw distance table
     * @return Reference to table (m × k floats)
     */
    const std::vector<std::vector<float>>& get_table() const { return table_; }

    /**
     * @brief Check if table has been built
     * @return true if built for a query
     */
    bool is_built() const { return built_; }

    /**
     * @brief Get table size in bytes
     * @return Size in bytes (m × k × sizeof(float))
     */
    Size get_table_size() const;

private:
    const PQEncoder& encoder_;
    bool built_;

    /// Distance lookup table: table_[subspace][centroid_idx] = partial_sq_distance
    /// Size: m × k floats = 8 × 256 × 4B = 8KB (fits in L1 cache)
    std::vector<std::vector<float>> table_;
};

}  // namespace agent_mem_io