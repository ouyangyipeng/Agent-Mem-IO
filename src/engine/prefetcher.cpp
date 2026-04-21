/**
 * @file prefetcher.cpp
 * @brief Topology-aware prefetcher implementation
 *
 * Implements the graph topology-aware "next-hop" prefetching logic:
 *   - When visiting a node during search, asynchronously prefetch
 *     its neighbors' vector data from SSD
 *   - Uses IoEngine (io_uring when available) for async I/O
 *   - Integrates with BufferPoolManager for cache-aware prefetch
 *   - Supports CPU-I/O overlap: issue prefetches while computing
 *     distances for the current batch
 */

#include "engine/storage_engine.h"
#include "io/disk_layout.h"
#include <algorithm>
#include <iostream>

namespace agent_mem_io {

// =============================================================================
// TopologyAwarePrefetcher — Enhanced Implementation
// =============================================================================

TopologyAwarePrefetcher::TopologyAwarePrefetcher(
    IoEngine* io_engine,
    BufferPoolManager* buffer_pool,
    const GraphIndex* graph_index
)
    : io_engine_(io_engine)
    , buffer_pool_(buffer_pool)
    , graph_index_(graph_index)
    , batch_size_(32)
{
}

/**
 * @brief Prefetch neighbors of a node using io_uring async I/O
 *
 * This is the core "next-hop" prefetching logic:
 *   1. Get neighbors of current node from in-memory graph
 *   2. For each neighbor not already in buffer pool:
 *      - Calculate the SSD offset for its vector data
 *      - Allocate an aligned buffer from buffer pool
 *      - Submit an async read via io_uring (or sync pread fallback)
 *   3. The prefetches complete in the background while CPU continues
 *      computing distances for the current batch
 *
 * @param node_id Current node being visited in graph search
 * @param vector_fd File descriptor for vector data on SSD
 * @return Number of prefetch requests submitted
 */
Size TopologyAwarePrefetcher::prefetch_neighbors(NodeId node_id, int vector_fd) {
    if (!graph_index_ || !io_engine_) {
        return 0;
    }

    const auto& neighbors = graph_index_->get_neighbors(node_id);
    if (neighbors.empty()) {
        return 0;
    }

    Size count = 0;
    std::vector<IoRequest> requests;
    requests.reserve(std::min(neighbors.size(), batch_size_));

    for (NodeId neighbor : neighbors) {
        if (count >= batch_size_) {
            break;
        }

        // Skip if already cached in buffer pool
        if (buffer_pool_ && buffer_pool_->contains(calculate_page_id(neighbor))) {
            cache_hits_++;
            continue;
        }

        // Calculate offset for this node's vector data
        Offset offset = calculate_vector_offset(neighbor);
        Size read_size = VECTOR_SIZE_BYTES;

        // Allocate aligned buffer for the read result
        char* buffer = nullptr;
        if (buffer_pool_) {
            // Use buffer pool's get_or_load_page with a load function
            // that just returns the buffer — the actual I/O happens via io_uring
            PageId page_id = calculate_page_id(neighbor);
            buffer = buffer_pool_->get_page(neighbor, page_id);
            if (buffer) {
                // Already in buffer pool (race condition: another query loaded it)
                cache_hits_++;
                continue;
            }
        }

        // For io_uring async reads, we need a valid aligned buffer
        // Allocate from aligned buffer pool (not BufferPoolManager pages)
        if (!buffer) {
            buffer = static_cast<char*>(malloc(read_size + ALIGNMENT));
            if (!buffer) continue;
            // Align the malloc'd buffer
            buffer = reinterpret_cast<char*>(
                (reinterpret_cast<uintptr_t>(buffer) + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
        }

        // Build IoRequest for async submission
        IoRequest req;
        req.fd = vector_fd;
        req.offset = offset;
        req.size = read_size;
        req.buffer = buffer;
        req.node_id = neighbor;
        req.is_read = true;
        req.user_data = static_cast<uint64_t>(neighbor);

        requests.push_back(req);
        count++;
        total_prefetches_++;
    }

    // Submit all prefetch requests as a batch (io_uring or sync fallback)
    if (!requests.empty() && io_engine_) {
        Size submitted = io_engine_->submit_batch_read(requests);
        if (submitted < requests.size()) {
            // Some requests failed — free their buffers
            for (Size i = submitted; i < requests.size(); ++i) {
                if (requests[i].buffer) {
                    free(requests[i].buffer);
                }
            }
        }
    }

    return count;
}

/**
 * @brief Wait for prefetch completions and insert results into buffer pool
 *
 * After issuing async prefetches, this function:
 *   1. Waits for io_uring completions (or returns immediately for sync I/O)
 *   2. For each successful completion, stores the data in BufferPoolManager
 *   3. Returns the number of successful completions
 *
 * @return Number of completed prefetch operations
 */
Size TopologyAwarePrefetcher::wait_prefetch_completions() {
    if (!io_engine_) {
        return 0;
    }

    Size count = 0;
    auto completions = io_engine_->wait_completion_batch(batch_size_, 100);

    for (const auto& comp : completions) {
        if (comp.success()) {
            count++;
            successful_prefetches_++;

            // Store in buffer pool if available
            if (buffer_pool_ && comp.buffer && comp.node_id != INVALID_NODE_ID) {
                PageId page_id = calculate_page_id(comp.node_id);

                // Load the prefetched data into buffer pool
                auto load_func = [&comp](char* buffer, PageId pid) {
                    // Copy prefetched data into buffer pool page
                    std::memcpy(buffer, comp.buffer, VECTOR_SIZE_BYTES);
                };

                buffer_pool_->get_or_load_page(comp.node_id, page_id, load_func);

                // Free the temporary prefetch buffer
                free(comp.buffer);
            }
        } else {
            // Prefetch failed — free buffer
            if (comp.buffer) {
                free(comp.buffer);
            }
        }
    }

    return count;
}

void TopologyAwarePrefetcher::get_stats(uint64_t& total_prefetches,
                                           uint64_t& successful_prefetches,
                                           uint64_t& cache_hits) const {
    total_prefetches = total_prefetches_;
    successful_prefetches = successful_prefetches_;
    cache_hits = cache_hits_;
}

void TopologyAwarePrefetcher::reset_stats() {
    total_prefetches_ = 0;
    successful_prefetches_ = 0;
    cache_hits_ = 0;
}

void TopologyAwarePrefetcher::set_batch_size(Size batch_size) {
    batch_size_ = batch_size;
}

}  // namespace agent_mem_io