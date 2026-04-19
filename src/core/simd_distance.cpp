/**
 * @file simd_distance.cpp
 * @brief SIMD-accelerated distance computation implementation
 *
 * Simplified approach: relies on -march=native compiler flag to enable
 * AVX2/SSE globally. Runtime detection selects the best available path.
 */

#include "core/simd_distance.h"

#include <cmath>
#include <cstring>

// On x86_64, include all SIMD intrinsics (enabled by -march=native)
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HAS_X86_SIMD 1
#else
#define HAS_X86_SIMD 0
#endif

namespace agent_mem_io {

// =============================================================================
// CPU Capability Detection
// =============================================================================

#if HAS_X86_SIMD

static void cpuid_func(int regs[4], int leaf) {
#if defined(_MSC_VER)
    __cpuid(regs, leaf);
#else
    __asm__ __volatile__(
        "cpuid"
        : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
        : "a"(leaf)
    );
#endif
}

bool has_avx2_support() {
    int regs[4];
    cpuid_func(regs, 7);
    return (regs[1] & (1 << 5)) != 0;
}

bool has_sse42_support() {
    int regs[4];
    cpuid_func(regs, 1);
    return (regs[2] & (1 << 20)) != 0;
}

#else

bool has_avx2_support() { return false; }
bool has_sse42_support() { return false; }

#endif

int get_simd_level() {
#if HAS_X86_SIMD
    if (has_avx2_support()) return 2;
    if (has_sse42_support()) return 1;
#endif
    return 0;
}

// =============================================================================
// Scalar Distance Computation (always available)
// =============================================================================

float l2_distance_sq_scalar(const float* a, const float* b, uint32_t dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

// =============================================================================
// AVX2 Distance Computation (x86_64 only, requires -mavx2 or -march=native)
// =============================================================================

#if HAS_X86_SIMD && defined(__AVX2__)

float l2_distance_sq_avx2(const float* a, const float* b, uint32_t dim) {
    __m256 sum_vec = _mm256_setzero_ps();

    uint32_t i = 0;
    const uint32_t main_end = dim - (dim % 8);
    for (; i < main_end; i += 8) {
        __m256 a_vec = _mm256_loadu_ps(a + i);
        __m256 b_vec = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(a_vec, b_vec);
        __m256 sq = _mm256_mul_ps(diff, diff);
        sum_vec = _mm256_add_ps(sum_vec, sq);
    }

    // Horizontal reduction
    __m128 hi_lane = _mm256_extractf128_ps(sum_vec, 1);
    __m128 lo_lane = _mm256_castps256_ps128(sum_vec);
    __m128 sum128 = _mm_add_ps(lo_lane, hi_lane);
    __m128 shuf = _mm_movehdup_ps(sum128);
    __m128 sums = _mm_add_ps(sum128, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    float total = _mm_cvtss_f32(sums);

    // Handle remaining elements
    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        total += diff * diff;
    }
    return total;
}

#else

float l2_distance_sq_avx2(const float* a, const float* b, uint32_t dim) {
    return l2_distance_sq_scalar(a, b, dim);
}

#endif  // AVX2

// =============================================================================
// SSE Distance Computation (x86_64 only, requires -msse4.2 or -march=native)
// =============================================================================

#if HAS_X86_SIMD && defined(__SSE4_2__)

float l2_distance_sq_sse(const float* a, const float* b, uint32_t dim) {
    __m128 sum_vec = _mm_setzero_ps();

    uint32_t i = 0;
    const uint32_t main_end = dim - (dim % 4);
    for (; i < main_end; i += 4) {
        __m128 a_vec = _mm_loadu_ps(a + i);
        __m128 b_vec = _mm_loadu_ps(b + i);
        __m128 diff = _mm_sub_ps(a_vec, b_vec);
        __m128 sq = _mm_mul_ps(diff, diff);
        sum_vec = _mm_add_ps(sum_vec, sq);
    }

    // Horizontal reduction
    __m128 shuf = _mm_movehdup_ps(sum_vec);
    __m128 sums = _mm_add_ps(sum_vec, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    float total = _mm_cvtss_f32(sums);

    // Handle remaining elements
    for (; i < dim; ++i) {
        float diff = a[i] - b[i];
        total += diff * diff;
    }
    return total;
}

#else

float l2_distance_sq_sse(const float* a, const float* b, uint32_t dim) {
    return l2_distance_sq_scalar(a, b, dim);
}

#endif  // SSE4.2

// =============================================================================
// Auto-selecting SIMD Distance
// =============================================================================

float l2_distance_sq_simd(const float* a, const float* b, uint32_t dim) {
    // Static initialization: detect once, use forever
    static int simd_lvl = -1;
    if (simd_lvl < 0) simd_lvl = get_simd_level();

    if (simd_lvl >= 2) return l2_distance_sq_avx2(a, b, dim);
    if (simd_lvl >= 1) return l2_distance_sq_sse(a, b, dim);
    return l2_distance_sq_scalar(a, b, dim);
}

float l2_distance_simd(const float* a, const float* b, uint32_t dim) {
    return std::sqrt(l2_distance_sq_simd(a, b, dim));
}

// =============================================================================
// Batch Distance Computation
// =============================================================================

void batch_l2_distance_sq_simd(const float* query, const float* database,
                                uint32_t num_vectors, uint32_t dim,
                                float* distances) {
    for (uint32_t i = 0; i < num_vectors; ++i) {
        distances[i] = l2_distance_sq_simd(query, database + i * dim, dim);
    }
}

}  // namespace agent_mem_io