/**
 * @file simd_distance.h
 * @brief SIMD-accelerated distance computation functions
 *
 * Provides AVX2 and SSE optimized L2 distance computation for 128-dim vectors.
 * Falls back to scalar computation on non-x86 platforms.
 *
 * Performance comparison (128-dim vectors):
 * - Scalar:  ~128 iterations, ~400ns per distance
 * - SSE:     ~32 iterations (4 floats per op), ~100ns per distance
 * - AVX2:    ~16 iterations (8 floats per op), ~50ns per distance
 */

#pragma once

#include "common/types.h"
#include <cstdint>
#include <cstddef>

namespace agent_mem_io {

// =============================================================================
// SIMD Distance Functions
// =============================================================================

/**
 * @brief Compute L2 squared distance between two vectors (best available SIMD)
 * @param a First vector (must be at least dim elements)
 * @param b Second vector (must be at least dim elements)
 * @param dim Vector dimension
 * @return L2 squared distance (||a - b||^2)
 *
 * Automatically selects AVX2 > SSE > scalar based on CPU capabilities.
 */
float l2_distance_sq_simd(const float* a, const float* b, uint32_t dim);

/**
 * @brief Compute L2 distance between two vectors (with sqrt)
 * @param a First vector
 * @param b Second vector
 * @param dim Vector dimension
 * @return L2 distance (sqrt of L2 squared)
 */
float l2_distance_simd(const float* a, const float* b, uint32_t dim);

/**
 * @brief Compute L2 squared distance using AVX2 (256-bit SIMD)
 * @param a First vector (must be aligned to 32 bytes for best performance)
 * @param b Second vector (must be aligned to 32 bytes for best performance)
 * @param dim Vector dimension (must be multiple of 8 for full AVX2 utilization)
 * @return L2 squared distance
 */
float l2_distance_sq_avx2(const float* a, const float* b, uint32_t dim);

/**
 * @brief Compute L2 squared distance using SSE (128-bit SIMD)
 * @param a First vector (must be aligned to 16 bytes for best performance)
 * @param b Second vector (must be aligned to 16 bytes for best performance)
 * @param dim Vector dimension (must be multiple of 4 for full SSE utilization)
 * @return L2 squared distance
 */
float l2_distance_sq_sse(const float* a, const float* b, uint32_t dim);

/**
 * @brief Compute L2 squared distance using scalar (fallback)
 * @param a First vector
 * @param b Second vector
 * @param dim Vector dimension
 * @return L2 squared distance
 */
float l2_distance_sq_scalar(const float* a, const float* b, uint32_t dim);

/**
 * @brief Batch compute L2 squared distances from one query to multiple vectors
 * @param query Query vector
 * @param database Database vectors (num_vectors × dim layout)
 * @param num_vectors Number of database vectors
 * @param dim Vector dimension
 * @param distances Output distance array (must have num_vectors elements)
 *
 * Much faster than calling l2_distance_sq_simd individually due to
 * better cache utilization and loop optimization.
 */
void batch_l2_distance_sq_simd(const float* query, const float* database,
                                uint32_t num_vectors, uint32_t dim,
                                float* distances);

// =============================================================================
// CPU Capability Detection
// =============================================================================

/**
 * @brief Check if AVX2 is available on the current CPU
 * @return true if AVX2 is supported
 */
bool has_avx2_support();

/**
 * @brief Check if SSE4.2 is available on the current CPU
 * @return true if SSE4.2 is supported
 */
bool has_sse42_support();

/**
 * @brief Get the best available SIMD level
 * @return 0=scalar, 1=SSE, 2=AVX2
 */
int get_simd_level();

}  // namespace agent_mem_io