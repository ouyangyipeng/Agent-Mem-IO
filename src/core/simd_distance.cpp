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

    // When pointers are 32-byte aligned (guaranteed by 4KB page layout with
    // DISK_RECORD_HEADER_SIZE=32), use aligned loads for better throughput.
    // _mm256_load_ps requires 32-byte alignment; _mm256_loadu_ps works for any alignment.
    // DiskIndexReader page buffers are 4KB-aligned, and vector data starts at offset 32,
    // so (page_base + 32) is always 32-byte aligned → safe for _mm256_load_ps.
    bool a_aligned = ((reinterpret_cast<uintptr_t>(a) & 0x1F) == 0);
    bool b_aligned = ((reinterpret_cast<uintptr_t>(b) & 0x1F) == 0);

    if (a_aligned && b_aligned) {
        for (; i < main_end; i += 8) {
            __m256 a_vec = _mm256_load_ps(a + i);   // Aligned load (32B boundary)
            __m256 b_vec = _mm256_load_ps(b + i);
            __m256 diff = _mm256_sub_ps(a_vec, b_vec);
            __m256 sq = _mm256_mul_ps(diff, diff);
            sum_vec = _mm256_add_ps(sum_vec, sq);
        }
    } else {
        for (; i < main_end; i += 8) {
            __m256 a_vec = _mm256_loadu_ps(a + i);  // Unaligned load (safe fallback)
            __m256 b_vec = _mm256_loadu_ps(b + i);
            __m256 diff = _mm256_sub_ps(a_vec, b_vec);
            __m256 sq = _mm256_mul_ps(diff, diff);
            sum_vec = _mm256_add_ps(sum_vec, sq);
        }
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

    // Use aligned loads when pointers are 16-byte aligned.
    // DiskIndexReader page buffers guarantee this: 4KB-aligned base + offset 32
    // → vector data pointer is 32-byte aligned (also 16-byte aligned for SSE).
    bool a_aligned = ((reinterpret_cast<uintptr_t>(a) & 0xF) == 0);
    bool b_aligned = ((reinterpret_cast<uintptr_t>(b) & 0xF) == 0);

    if (a_aligned && b_aligned) {
        for (; i < main_end; i += 4) {
            __m128 a_vec = _mm_load_ps(a + i);   // Aligned load (16B boundary)
            __m128 b_vec = _mm_load_ps(b + i);
            __m128 diff = _mm_sub_ps(a_vec, b_vec);
            __m128 sq = _mm_mul_ps(diff, diff);
            sum_vec = _mm_add_ps(sum_vec, sq);
        }
    } else {
        for (; i < main_end; i += 4) {
            __m128 a_vec = _mm_loadu_ps(a + i);  // Unaligned load (safe fallback)
            __m128 b_vec = _mm_loadu_ps(b + i);
            __m128 diff = _mm_sub_ps(a_vec, b_vec);
            __m128 sq = _mm_mul_ps(diff, diff);
            sum_vec = _mm_add_ps(sum_vec, sq);
        }
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
    // Optimized batch: software prefetch + loop unroll for better cache utilization.
    // Prefetching the next vector while computing the current one hides L2 cache latency.
    const uint32_t UNROLL = 4;  // Process 4 vectors per iteration
    uint32_t i = 0;
    const uint32_t unroll_end = num_vectors - (num_vectors % UNROLL);

    for (; i < unroll_end; i += UNROLL) {
        // Software prefetch next group of vectors into L2 cache
        if (i + UNROLL < num_vectors) {
            __builtin_prefetch(database + (i + UNROLL) * dim, 0, 1);
        }
        // Compute distances for current group
        distances[i]     = l2_distance_sq_simd(query, database + i * dim, dim);
        distances[i + 1] = l2_distance_sq_simd(query, database + (i + 1) * dim, dim);
        distances[i + 2] = l2_distance_sq_simd(query, database + (i + 2) * dim, dim);
        distances[i + 3] = l2_distance_sq_simd(query, database + (i + 3) * dim, dim);
    }

    // Handle remaining vectors
    for (; i < num_vectors; ++i) {
        distances[i] = l2_distance_sq_simd(query, database + i * dim, dim);
    }
}

}  // namespace agent_mem_io