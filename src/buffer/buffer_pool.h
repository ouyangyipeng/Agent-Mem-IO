/**
 * @file buffer_pool.h
 * @brief User-space buffer pool manager for O_DIRECT I/O
 * 
 * This file implements a custom buffer pool that bypasses the OS Page Cache
 * and manages memory directly for vector data caching. The implementation
 * uses a Graph-Aware 2Q eviction policy optimized for graph traversal patterns.
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <list>
#include <condition_variable>
#include <functional>

namespace agent_mem_io {

/**
 * @brief Buffer pool configuration
 */
struct BufferPoolConfig {
    Size max_pages = DEFAULT_BUFFER_POOL_PAGES;  // Maximum number of pages
    Size page_size = PAGE_SIZE;                   // Page size (must be aligned)
    double hot_queue_ratio = 0.3;                 // Ratio of pages in hot queue
    bool enable_graph_aware = true;               // Enable graph-aware eviction
    bool enable_prefetch = true;                  // Enable prefetching
    
    BufferPoolConfig() = default;
};

/**
 * @brief Page frame structure
 * 
 * Represents a single page in the buffer pool, containing
 * vector data and metadata.
 */
struct PageFrame {
    PageId page_id;                    // Page identifier
    NodeId node_id;                    // Associated node ID
    char* data;                        // Page data (aligned buffer)
    bool is_valid;                     // Whether page contains valid data
    bool is_dirty;                     // Whether page has been modified
    bool is_pinned;                    // Whether page is pinned (cannot be evicted)
    uint32_t pin_count;                // Number of pins
    uint64_t access_count;             // Number of accesses
    uint64_t last_access_time;         // Last access timestamp
    uint32_t in_degree;                // Node's in-degree (for graph-aware eviction)
    
    PageFrame()
        : page_id(INVALID_PAGE_ID)
        , node_id(INVALID_NODE_ID)
        , data(nullptr)
        , is_valid(false)
        , is_dirty(false)
        , is_pinned(false)
        , pin_count(0)
        , access_count(0)
        , last_access_time(0)
        , in_degree(0)
    {}
};

/**
 * @brief 2Q eviction policy
 * 
 * Implements the 2Q (Two Queues) algorithm with graph-aware optimization.
 * The algorithm maintains two queues:
 * - Hot queue: Frequently accessed pages (LRU eviction)
 * - Warm queue: Recently accessed pages (FIFO eviction)
 * 
 * Graph-aware optimization: Pages with high in-degree are prioritized
 * in the hot queue, as they are more likely to be accessed again.
 */
class TwoQueueEvictionPolicy {
public:
    /**
     * @brief Construct 2Q eviction policy
     * @param max_pages Maximum number of pages
     * @param hot_queue_ratio Ratio of pages in hot queue
     */
    TwoQueueEvictionPolicy(Size max_pages, double hot_queue_ratio);
    
    /**
     * @brief Record page access
     * @param page_id Page identifier
     * @param in_degree Node's in-degree (for graph-aware optimization)
     */
    void on_access(PageId page_id, uint32_t in_degree);
    
    /**
     * @brief Add new page to policy
     * @param page_id Page identifier
     * @param in_degree Node's in-degree
     */
    void on_add(PageId page_id, uint32_t in_degree);
    
    /**
     * @brief Remove page from policy
     * @param page_id Page identifier
     */
    void on_remove(PageId page_id);
    
    /**
     * @brief Get page to evict
     * @return Page ID to evict, or INVALID_PAGE_ID if no page can be evicted
     */
    PageId get_eviction_candidate();
    
    /**
     * @brief Check if page is in hot queue
     * @param page_id Page identifier
     * @return true if in hot queue
     */
    bool is_in_hot_queue(PageId page_id) const;
    
    /**
     * @brief Check if page is in warm queue
     * @param page_id Page identifier
     * @return true if in warm queue
     */
    bool is_in_warm_queue(PageId page_id) const;
    
    /**
     * @brief Get number of pages in hot queue
     * @return Number of pages
     */
    Size hot_queue_size() const;
    
    /**
     * @brief Get number of pages in warm queue
     * @return Number of pages
     */
    Size warm_queue_size() const;
    
    /**
     * @brief Update in-degree for a page
     * @param page_id Page identifier
     * @param in_degree New in-degree
     */
    void update_in_degree(PageId page_id, uint32_t in_degree);
    
    /**
     * @brief Clear all queues
     */
    void clear();

private:
    /**
     * @brief Promote page from warm to hot queue
     * @param page_id Page identifier
     */
    void promote_to_hot_queue(PageId page_id);
    
    /**
     * @brief Evict from hot queue using LRU
     * @return Page ID to evict
     */
    PageId evict_from_hot_queue();
    
    /**
     * @brief Evict from warm queue using FIFO
     * @return Page ID to evict
     */
    PageId evict_from_warm_queue();
    
    Size max_pages_;
    Size hot_queue_max_size_;
    
    // Hot queue: LRU list (most recently used at front)
    std::list<PageId> hot_queue_;
    std::unordered_map<PageId, std::list<PageId>::iterator> hot_queue_map_;
    
    // Warm queue: FIFO list (oldest at front)
    std::list<PageId> warm_queue_;
    std::unordered_map<PageId, std::list<PageId>::iterator> warm_queue_map_;
    
    // In-degree map for graph-aware optimization
    std::unordered_map<PageId, uint32_t> in_degree_map_;
    
    mutable std::mutex mutex_;
};

/**
 * @brief Buffer pool manager
 * 
 * Manages a pool of memory pages for caching vector data from SSD.
 * Uses O_DIRECT for all I/O operations to bypass OS Page Cache.
 */
class BufferPoolManager {
public:
    /**
     * @brief Construct buffer pool manager
     * @param config Buffer pool configuration
     */
    explicit BufferPoolManager(const BufferPoolConfig& config);
    
    /**
     * @brief Destructor - frees all allocated memory
     */
    ~BufferPoolManager();
    
    /**
     * @brief Get page from buffer pool
     * @param node_id Node identifier
     * @param page_id Page identifier
     * @return Pointer to page data, or nullptr if not found
     */
    char* get_page(NodeId node_id, PageId page_id);
    
    /**
     * @brief Get page or load from disk if not present
     * @param node_id Node identifier
     * @param page_id Page identifier
     * @param load_func Function to load page from disk
     * @return Pointer to page data
     */
    char* get_or_load_page(
        NodeId node_id,
        PageId page_id,
        std::function<void(char* buffer, PageId page_id)> load_func
    );
    
    /**
     * @brief Directly insert page data into buffer pool (no load_func needed)
     *
     * Used when data is already available (e.g., from async I/O completion).
     * If page already exists, returns existing data (hit).
     * If not, allocates a frame, copies data, and registers with eviction policy.
     *
     * @param node_id Node identifier
     * @param page_id Page identifier
     * @param data Source data to copy into the page
     * @param data_size Size of data to copy
     * @return Pointer to page data, or nullptr if buffer pool full
     */
    char* put_page_data(NodeId node_id, PageId page_id,
                        const char* data, Size data_size);
    
    /**
     * @brief Pin a page (prevent eviction)
     * @param page_id Page identifier
     * @return true if page was pinned
     */
    bool pin_page(PageId page_id);
    
    /**
     * @brief Unpin a page (allow eviction)
     * @param page_id Page identifier
     * @return true if page was unpinned
     */
    bool unpin_page(PageId page_id);
    
    /**
     * @brief Mark page as dirty
     * @param page_id Page identifier
     */
    void mark_dirty(PageId page_id);
    
    /**
     * @brief Flush dirty pages to disk
     * @param flush_func Function to flush page to disk
     * @return Number of pages flushed
     */
    Size flush_dirty_pages(std::function<void(const char* buffer, PageId page_id)> flush_func);
    
    /**
     * @brief Check if page is in buffer pool
     * @param page_id Page identifier
     * @return true if page is present
     */
    bool contains(PageId page_id) const;
    
    /**
     * @brief Get number of pages in buffer pool
     * @return Number of pages
     */
    Size size() const;
    
    /**
     * @brief Get maximum number of pages
     * @return Maximum pages
     */
    Size max_size() const;
    
    /**
     * @brief Get cache hit rate
     * @return Cache hit rate (0.0 to 1.0)
     */
    double get_hit_rate() const;
    
    /**
     * @brief Get number of cache hits
     * @return Number of cache hits
     */
    uint64_t get_hit_count() const;
    
    /**
     * @brief Get number of cache misses
     * @return Number of cache misses
     */
    uint64_t get_miss_count() const;
    
    /**
     * @brief Reset statistics
     */
    void reset_stats();
    
    /**
     * @brief Get memory usage
     * @return Memory usage in bytes
     */
    Size get_memory_usage() const;
    
    /**
     * @brief Update in-degree for a node
     * @param node_id Node identifier
     * @param in_degree New in-degree
     */
    void update_in_degree(NodeId node_id, uint32_t in_degree);
    
    /**
     * @brief Prefetch pages (async load)
     * @param page_ids List of page IDs to prefetch
     * @param load_func Function to load page from disk
     */
    void prefetch_pages(
        const std::vector<PageId>& page_ids,
        std::function<void(char* buffer, PageId page_id)> load_func
    );
    
    /**
     * @brief Clear buffer pool
     */
    void clear();

private:
    /**
     * @brief Allocate aligned buffer
     * @param size Buffer size
     * @return Pointer to aligned buffer
     */
    char* allocate_aligned_buffer(Size size);
    
    /**
     * @brief Free aligned buffer
     * @param buffer Buffer to free
     */
    void free_aligned_buffer(char* buffer);
    
    /**
     * @brief Find a free frame or evict one
     * @return Frame index, or -1 if no frame available
     */
    int find_free_frame();
    
    /**
     * @brief Evict a page
     * @param flush_func Function to flush dirty page
     * @return true if eviction succeeded
     */
    bool evict_page(std::function<void(const char* buffer, PageId page_id)> flush_func = nullptr);
    
    BufferPoolConfig config_;
    
    // Page frames
    std::vector<PageFrame> frames_;
    
    // Page table: page_id -> frame index
    std::unordered_map<PageId, int> page_table_;
    
    // Free frame list
    std::list<int> free_frames_;
    
    // Eviction policy
    std::unique_ptr<TwoQueueEvictionPolicy> eviction_policy_;
    
    // Statistics
    std::atomic<uint64_t> hit_count_{0};
    std::atomic<uint64_t> miss_count_{0};
    
    // Mutex for thread-safe access
    mutable std::shared_mutex mutex_;
    
    // Condition variable for prefetch
    std::condition_variable prefetch_cv_;
    std::mutex prefetch_mutex_;
};

/**
 * @brief Aligned buffer helper class
 * 
 * Provides aligned memory allocation for O_DIRECT I/O operations.
 */
class AlignedBuffer {
public:
    /**
     * @brief Construct aligned buffer
     * @param size Buffer size (will be rounded up to alignment)
     */
    explicit AlignedBuffer(Size size);
    
    /**
     * @brief Destructor
     */
    ~AlignedBuffer();
    
    /**
     * @brief Get buffer pointer
     * @return Pointer to aligned buffer
     */
    char* data() { return data_; }
    
    /**
     * @brief Get buffer pointer (const)
     * @return Pointer to aligned buffer
     */
    const char* data() const { return data_; }
    
    /**
     * @brief Get buffer size
     * @return Buffer size
     */
    Size size() const { return size_; }
    
    /**
     * @brief Check if buffer is valid
     * @return true if buffer is allocated
     */
    bool valid() const { return data_ != nullptr; }
    
    // Prevent copying
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    
    // Allow moving
    AlignedBuffer(AlignedBuffer&& other) noexcept;
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;

private:
    char* data_;
    Size size_;
};

}  // namespace agent_mem_io