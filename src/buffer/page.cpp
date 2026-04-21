/**
 * @file page.cpp
 * @brief Page utilities — alignment, checksum, and zero-fill helpers
 *
 * Provides page-level utility functions used by BufferPoolManager for
 * validating, initializing, and computing checksums on 4KB-aligned pages.
 * Core page lifecycle (alloc/free/replace) remains in buffer_pool.cpp.
 */

#include "buffer/buffer_pool.h"
#include "common/types.h"
#include <cstring>
#include <cstdlib>

namespace agent_mem_io {

// =============================================================================
// Page Alignment Utilities
// =============================================================================

Size align_to_page_size(Size size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
}

bool is_page_aligned(const void* ptr) {
    return (reinterpret_cast<uintptr_t>(ptr) % PAGE_SIZE) == 0;
}

bool is_page_aligned_offset(Offset offset) {
    return (offset % PAGE_SIZE) == 0;
}

// =============================================================================
// Page Content Utilities
// =============================================================================

void zero_page(char* page) {
    std::memset(page, 0, PAGE_SIZE);
}

uint32_t compute_page_checksum(const char* page) {
    // Simple Fletcher-16 checksum over the first 4092 bytes
    // (last 4 bytes reserved for the checksum itself)
    uint16_t sum1 = 0;
    uint16_t sum2 = 0;

    for (Size i = 0; i < PAGE_SIZE - sizeof(uint32_t); ++i) {
        sum1 = (sum1 + static_cast<uint16_t>(page[i])) % 255;
        sum2 = (sum2 + sum1) % 255;
    }

    return (static_cast<uint32_t>(sum2) << 8) | static_cast<uint32_t>(sum1);
}

void write_page_checksum(char* page) {
    uint32_t cs = compute_page_checksum(page);
    std::memcpy(page + PAGE_SIZE - sizeof(uint32_t), &cs, sizeof(cs));
}

bool verify_page_checksum(const char* page) {
    uint32_t stored_cs;
    std::memcpy(&stored_cs, page + PAGE_SIZE - sizeof(uint32_t), sizeof(stored_cs));
    uint32_t computed_cs = compute_page_checksum(page);
    return stored_cs == computed_cs;
}

}  // namespace agent_mem_io