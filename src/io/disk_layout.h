/**
 * @file disk_layout.h
 * @brief Disk-resident node record layout for O_DIRECT SSD access
 *
 * Defines the on-disk format for vector + graph + PQ data, and provides
 * reader/writer classes with:
 *   - 4KB-aligned O_DIRECT I/O (SSD-friendly, bypasses OS Page Cache)
 *   - Batch reads via preadv (multiple nodes in fewer syscalls)
 *   - Async batch reads via io_uring (CPU-I/O overlap for graph traversal)
 *   - Graph-Aware 2Q BufferPool (reduce SSD reads for hub nodes)
 *   - Pre-allocated buffer pool (eliminate per-read alloc/dealloc overhead)
 */

#pragma once

#include "common/types.h"
#include "core/pq_encoder.h"
#include "io/io_uring_engine.h"
#include "buffer/buffer_pool.h"

#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>

namespace agent_mem_io {

// =============================================================================
// Constants
// =============================================================================

/// Disk record size: fixed 4KB for O_DIRECT alignment and SSD page size
constexpr Size DISK_RECORD_SIZE = 4096;

/// Maximum graph degree stored on disk (room for 32 neighbors)
constexpr Size DEFAULT_DISK_MAX_DEGREE = 32;

/// Header size: node_id (4B) + 28B padding for full SIMD alignment
/// Vector data starts at offset 32 within each 4KB page, ensuring
/// 32-byte alignment for AVX2 _mm256_load_ps (4KB page base is
/// aligned to 4096, and 4096+32 is 32-byte aligned). This enables
/// aligned SIMD loads directly from BufferPool page buffers without
/// memcpy, a key optimization for compute_distance_direct().
constexpr Size DISK_RECORD_HEADER_SIZE = 32;

/// Offset where vector data begins within a disk record
/// This is aligned to 16 bytes for SIMD access directly from page buffer.
constexpr Size DISK_VECTOR_OFFSET = DISK_RECORD_HEADER_SIZE;

/// Vector data size: 128 floats
constexpr Size DISK_VECTOR_DATA_SIZE = DEFAULT_DIMENSION * sizeof(float);

// =============================================================================
// DiskNodeRecord - Parsed from 4KB disk block
// =============================================================================

/**
 * @brief Parsed disk node record containing vector + neighbor info + PQ codes
 *
 * This struct is what we get after parsing a raw 4KB disk block.
 * It's used for cache entries and beam-style search.
 */
struct DiskNodeRecord {
    NodeId node_id;
    /// Vector data aligned to 16 bytes for SSE/AVX2 direct access.
    /// When populated from a page buffer, the source is also 16-byte aligned
    /// (DISK_VECTOR_OFFSET = 16 within a 4KB-aligned page).
    alignas(16) float vector_data[DEFAULT_DIMENSION];  // 128 floats
    Size num_neighbors;                      // actual neighbor count
    NodeId neighbor_ids[DEFAULT_DISK_MAX_DEGREE];  // up to 32 neighbors
    PQCode neighbor_pq_codes[DEFAULT_DISK_MAX_DEGREE * DEFAULT_PQ_M];  // up to 32×8=256 bytes

    /**
     * @brief Get total used bytes in this record
     */
    Size used_bytes() const {
        return DISK_RECORD_HEADER_SIZE
             + DISK_VECTOR_DATA_SIZE
             + sizeof(Size)  // num_neighbors field
             + num_neighbors * sizeof(NodeId)  // neighbor IDs
             + num_neighbors * DEFAULT_PQ_M;   // neighbor PQ codes
    }

    /**
     * @brief Get a neighbor's PQ code vector
     * @param neighbor_idx Index into neighbor_ids array (0 to num_neighbors-1)
     * @return PQ code vector of size m
     */
    PQCodeVector get_neighbor_pq_code(Size neighbor_idx) const {
        PQCodeVector codes(DEFAULT_PQ_M);
        for (Size j = 0; j < DEFAULT_PQ_M; ++j) {
            codes[j] = neighbor_pq_codes[neighbor_idx * DEFAULT_PQ_M + j];
        }
        return codes;
    }

    /**
     * @brief Calculate utilization ratio
     * @return Fraction of 4KB that is useful data
     */
    float utilization_ratio() const {
        return static_cast<float>(used_bytes()) / static_cast<float>(DISK_RECORD_SIZE);
    }

    /// Size of this struct (for cache memory accounting)
    static constexpr Size struct_size() {
        return sizeof(NodeId) + sizeof(float) * DEFAULT_DIMENSION
             + sizeof(Size) + sizeof(NodeId) * DEFAULT_DISK_MAX_DEGREE
             + sizeof(PQCode) * DEFAULT_DISK_MAX_DEGREE * DEFAULT_PQ_M;
    }
};

// =============================================================================
// DiskIndexWriter
// =============================================================================

/**
 * @brief Writes vector dataset + graph + PQ codes to SSD in DiskANN format
 *
 * Creates a single file where each node occupies exactly 4KB, containing
 * its full-precision vector, neighbor IDs, and neighbor PQ codes.
 * All writes use O_DIRECT for alignment requirements.
 */
class DiskIndexWriter {
public:
    /**
     * @brief Construct disk index writer
     * @param data_dir Directory to write index files
     * @param num_vectors Number of vectors in dataset
     * @param max_degree Maximum graph degree
     * @param pq_encoder PQ encoder (must be trained)
     */
    DiskIndexWriter(const std::string& data_dir,
                    Size num_vectors,
                    Size max_degree = DEFAULT_DISK_MAX_DEGREE,
                    const PQEncoder* pq_encoder = nullptr);

    /**
     * @brief Write all node records to disk
     * @param vectors Full-precision vector dataset
     * @param graph Neighbor lists (node_id -> list of neighbors)
     * @param pq_codes PQ-encoded vectors (for neighbor PQ codes in records)
     * @return Number of bytes written
     */
    Size write_index(const std::vector<Vector>& vectors,
                     const std::vector<std::vector<NodeId>>& graph,
                     const std::vector<PQCodeVector>& pq_codes);

    /**
     * @brief Get the index file path
     */
    const std::string& get_index_path() const { return index_path_; }

private:
    std::string data_dir_;
    Size num_vectors_;
    Size max_degree_;
    const PQEncoder* pq_encoder_;
    std::string index_path_;

    /**
     * @brief Pack a node's data into a 4KB record buffer
     */
    void pack_record(const Vector& vector,
                     const std::vector<NodeId>& neighbors,
                     const std::vector<PQCodeVector>& pq_codes,
                     NodeId node_id,
                     char* buffer);
};

// =============================================================================
// DiskIndexReader - Enhanced with batch reads + cache + buffer pool
// =============================================================================

/**
 * @brief Reads node records from SSD using O_DIRECT
 *
 * Enhanced version with:
 *   - Batch reads via preadv (read multiple 4KB records in fewer syscalls)
 *   - Async batch reads via io_uring (CPU-I/O overlap for graph traversal)
 *   - Graph-Aware 2Q buffer pool (protects hub nodes from eviction)
 *   - Pre-allocated temp buffer pool (eliminate per-read alloc/dealloc overhead)
 *
 * The cache is critical for graph traversal: hub nodes (entry point,
 * high-degree nodes) are accessed repeatedly across queries.
 * With 10-20% memory budget, we can cache ~5-15% of all vectors,
 * achieving 30-60% cache hit rate for typical NSW traversals.
 */
class DiskIndexReader {
public:
    /**
     * @brief Construct disk index reader
     * @param data_dir Directory containing index files
     * @param num_vectors Number of vectors
     * @param max_degree Maximum graph degree
     */
    DiskIndexReader(const std::string& data_dir,
                    Size num_vectors,
                    Size max_degree = DEFAULT_DISK_MAX_DEGREE);

    /**
     * @brief Destructor - frees buffer pool and closes file
     */
    ~DiskIndexReader();

    /**
     * @brief Open the index file with O_DIRECT
     * @return true if opened successfully
     */
    bool open();

    /**
     * @brief Close the index file
     */
    void close();

    // --- Single-node reads (existing API, enhanced with cache) ---

    /**
     * @brief Read a single node record from SSD (synchronous, O_DIRECT)
     * @param node_id Node to read
     * @param buffer 4KB-aligned buffer (must be from borrow_buffer())
     * @return true if read succeeded
     */
    bool read_node(NodeId node_id, char* buffer);

    /**
     * @brief Parse a raw 4KB buffer into a DiskNodeRecord
     * @param buffer Raw 4KB buffer from read_node
     * @return Parsed DiskNodeRecord
     */
    DiskNodeRecord parse_record(const char* buffer);

    /**
     * @brief Read a node's full-precision vector (cache-aware)
     *
     * Checks cache first. If cached, returns immediately (no SSD read).
     * If not cached, reads from SSD and adds to cache.
     *
     * @param node_id Node to read
     * @return Vector data
     */
    Vector read_vector(NodeId node_id);

    /**
     * @brief Read a node's full DiskNodeRecord (cache-aware)
     *
     * Checks cache first. If cached, returns immediately.
     * If not cached, reads from SSD, parses, and caches.
     *
     * @param node_id Node to read
     * @return Parsed DiskNodeRecord
     */
    DiskNodeRecord read_record(NodeId node_id);

    // --- Async I/O via io_uring (CPU-I/O overlap for topology-aware prefetch) ---
    
    /**
     * @brief Set IoEngine for async batch reads (io_uring when available)
     * @param io_engine Pointer to IoEngine instance
     *
     * When set, read_vectors_async() will use io_uring for truly async I/O,
     * enabling CPU-I/O overlap during graph traversal search.
     */
    void set_io_engine(IoEngine* io_engine);
    
    /**
     * @brief Issue async batch read for multiple node IDs
     *
     * Submits all reads to IoEngine (io_uring if available) and returns
     * immediately. The caller can continue CPU work while I/O completes
     * in the background. Call wait_async_batch() to retrieve results.
     *
     * This enables the key "CPU-I/O overlap" pattern:
     *   1. While computing distances for current batch,
     *      async reads for next batch are completing on SSD
     *   2. When current batch is done, next batch is already in memory
     *
     * @param node_ids List of node IDs to read (cache misses only)
     * @return Number of async reads submitted
     */
    Size submit_async_batch(const std::vector<NodeId>& node_ids);
    
    /**
     * @brief Wait for async batch reads to complete and parse results
     *
     * Polls IoEngine for completed reads, parses them into DiskNodeRecords,
     * stores in cache, and returns vectors for all completed reads.
     *
     * @param node_ids The same node_ids passed to submit_async_batch()
     * @param output_vectors Output vectors (resized to match node_ids)
     * @return Number of nodes successfully read
     */
    Size wait_async_batch(const std::vector<NodeId>& node_ids,
                          std::vector<Vector>& output_vectors);
    
    /**
     * @brief Check if io_engine is set and using io_uring
     */
    bool using_io_uring() const;

    // --- Batch reads (synchronous, for fallback and testing) ---

    /**
     * @brief Batch read multiple node vectors from SSD using preadv
     *
     * For nodes already in cache, skips SSD read.
     * For cache misses, issues batch SSD reads then caches results.
     *
     * @param node_ids List of node IDs to read
     * @param output_vectors Output vectors (resized to match node_ids)
     * @return Number of nodes successfully read (including cache hits)
     */
    Size read_vectors_batch(const std::vector<NodeId>& node_ids,
                            std::vector<Vector>& output_vectors);

    /**
     * @brief Batch read multiple DiskNodeRecords (cache-aware)
     *
     * @param node_ids List of node IDs to read
     * @param output_records Output records
     * @return Number of nodes successfully read
     */
    Size read_records_batch(const std::vector<NodeId>& node_ids,
                            std::vector<DiskNodeRecord>& output_records);

    // --- Graph-Aware 2Q Buffer Pool Cache ---

    /**
     * @brief Configure cache capacity using Graph-Aware 2Q BufferPoolManager
     * @param max_pages Maximum number of 4KB pages to cache (0 = disabled)
     *
     * Uses BufferPoolManager with Graph-Aware 2Q eviction policy
     * that protects high in-degree hub nodes from eviction.
     */
    void set_cache_capacity(Size max_pages);

    /**
     * @brief Update in-degrees for all cached nodes (Graph-Aware 2Q optimization)
     * @param in_degrees Vector where in_degrees[node_id] = node's in-degree
     *
     * Must be called after graph construction to enable Graph-Aware eviction.
     * High in-degree nodes (hub nodes) are protected from eviction, ensuring
     * they stay cached for future queries.
     */
    void update_in_degrees(const std::vector<uint32_t>& in_degrees);

    /**
     * @brief Update the dynamic hub threshold for Graph-Aware 2Q eviction
     * @param in_degrees In-degree distribution from the graph index
     *
     * Computes a percentile-based threshold from the in-degree distribution
     * and updates the eviction policy. Nodes with in_degree >= threshold
     * are classified as "hub nodes" and protected from eviction.
     */
    void update_buffer_pool_hub_threshold(const std::vector<uint32_t>& in_degrees);

    /**
     * @brief Check if a node is cached in the buffer pool
     */
    bool is_cached(NodeId node_id) const;

    /**
     * @brief Get cached record parsed from buffer pool (no SSD read)
     * @return Parsed DiskNodeRecord, or default record if not cached
     */
    DiskNodeRecord get_cached_record(NodeId node_id);

    /**
     * @brief Compute L2 squared distance directly from BufferPool page buffer
     *
     * Key optimization: skips parse_record memcpy. Vector data is at
     * DISK_VECTOR_OFFSET (16 bytes) within each 4KB-aligned page,
     * which is 16-byte aligned for SSE _mm_load_ps.
     *
     * When the page is cached in BufferPool, this computes distance
     * directly from the page buffer without copying to a DiskNodeRecord.
     * When not cached, falls back to read_vector + l2_distance.
     *
     * @param node_id Target node whose vector to compare
     * @param query Query vector
     * @return L2 squared distance, or -1.0f on error
     */
    float compute_distance_direct(NodeId node_id, const Vector& query);

    /**
     * @brief Ensure a node's page is cached AND pinned (atomic load+pin)
     *
     * Critical for multi-threaded search: atomically loads the page into
     * the BufferPool cache AND pins it under the same lock. This prevents
     * the data race where a page could be evicted between loading and pinning.
     *
     * After this call, the page is guaranteed to remain cached until
     * unpin_page() or unpin_all_pages() is called. This provides
     * search-level pin protection: compute_distance_direct() internally
     * does micro-level pin/unpin (pin_count goes 1→2→1), but the page
     * stays pinned at pin_count=1 after micro-level unpin.
     *
     * @param node_id Node whose page to ensure is cached and pinned
     * @return true if page is now cached and pinned
     */
    bool ensure_page_pinned(NodeId node_id);

    /**
     * @brief Pin a page in the buffer pool (prevent eviction during search)
     *
     * Forwarded to BufferPoolManager::pin_page(). Pinned pages cannot be
     * evicted by the 2Q policy, ensuring they remain accessible during
     * multi-threaded search. Must be paired with unpin_page() after use.
     *
     * @param node_id Node whose page to pin
     * @return true if page was pinned (page must already be in cache)
     */
    bool pin_page(NodeId node_id);

    /**
     * @brief Unpin a page in the buffer pool (allow eviction again)
     *
     * Forwarded to BufferPoolManager::unpin_page(). Must be called after
     * pin_page() when the page is no longer needed by the search.
     *
     * @param node_id Node whose page to unpin
     * @return true if page was unpinned
     */
    bool unpin_page(NodeId node_id);

    /**
     * @brief Unpin all pages in a list (bulk operation, single lock)
     *
     * More efficient than calling unpin_page() individually for each page.
     * Used at the end of diskann_search_enhanced() to release all pages
     * pinned during a single search query.
     *
     * @param node_ids List of node IDs whose pages to unpin
     */
    void unpin_all_pages(const std::vector<NodeId>& node_ids);

    /**
     * @brief Batch compute L2 squared distances directly from BufferPool
     *
     * For each node_id: if cached, compute distance directly from page buffer;
     * if not cached, read via BufferPool (which triggers I/O) then compute.
     * Adds software prefetch for next vector to improve cache utilization.
     *
     * @param node_ids Target node IDs
     * @param query Query vector
     * @param distances Output distances (must have node_ids.size() elements)
     * @return Number of successful distance computations
     */
    Size compute_distances_batch_direct(
        const std::vector<NodeId>& node_ids,
        const Vector& query,
        std::vector<float>& distances);

    /**
     * @brief Get cache hit rate from buffer pool
     */
    double get_cache_hit_rate() const;

    /**
     * @brief Get cache memory usage (bytes occupied by buffer pool pages)
     */
    Size get_cache_memory_usage() const;

    /**
     * @brief Get cache size (number of pages in buffer pool)
     */
    Size get_cache_size() const;

    /**
     * @brief Get cache capacity (maximum number of pages)
     */
    Size get_cache_capacity() const;

    // --- Buffer Pool (NEW) ---

    /**
     * @brief Borrow a pre-allocated 4KB-aligned buffer from the pool
     *
     * Avoids posix_memalign/free overhead per read.
     * Caller must return_buffer() after use.
     *
     * @return 4KB-aligned buffer, or nullptr if pool exhausted
     */
    char* borrow_buffer();

    /**
     * @brief Return a borrowed buffer to the pool for reuse
     * @param buffer Buffer previously borrowed
     */
    void return_buffer(char* buffer);

    /**
     * @brief Get I/O buffer pool size (number of available temp buffers)
     */
    Size get_io_buffer_pool_size() const { return io_buffer_pool_.size(); }

    // --- Utility ---

    /**
     * @brief Calculate file offset for a node
     * @param node_id Node ID
     * @return Byte offset in the index file
     */
    Size calculate_offset(NodeId node_id) const;

    /**
     * @brief Get index file path
     */
    const std::string& get_index_path() const { return index_path_; }

    /**
     * @brief Check if file is open
     */
    bool is_open() const { return fd_ >= 0; }

private:
    std::string data_dir_;
    Size num_vectors_;
    Size max_degree_;
    std::string index_path_;
    int fd_;  // File descriptor (opened with O_DIRECT)

    /// Graph-Aware 2Q page cache manager.
    /// Caches SSD pages in RAM with eviction policy (2Q + in-degree protection).
    /// Used for compute_distance_direct() and compute_distances_batch_direct().
    std::unique_ptr<BufferPoolManager> buffer_pool_mgr_;

    /// Pre-allocated temporary I/O buffer pool for async batch reads.
    /// These are short-lived 4KB O_DIRECT-aligned buffers, borrowed for a single
    /// read operation and returned after use. NOT a cache — no eviction policy.
    /// Protected by io_buffer_pool_mutex_ for thread-safe borrow/return.
    std::vector<char*> io_buffer_pool_;
    std::mutex io_buffer_pool_mutex_;    // Protects io_buffer_pool_ for multi-threaded access
    Size io_buffer_pool_capacity_ = 64;  // Default: 64 pre-allocated temp buffers

    /// IoEngine for async reads (io_uring when available, pread fallback)
    IoEngine* io_engine_ = nullptr;

    /// io_uring fixed-buffer indices for registered io_buffer_pool_ entries
    /// These are registered in set_io_engine() and unregistered in close()
    std::vector<int> registered_buffer_indices_;

    /// Pending async read buffers (node_id -> aligned buffer)
    /// These buffers are allocated when submitting async reads and
    /// freed after parsing results in wait_async_batch()
    std::unordered_map<NodeId, char*> async_buffers_;

    /// Mutex protecting async_buffers_ for multi-threaded io_uring access
    std::mutex async_buffers_mutex_;

    /// Allocate a new aligned buffer (for pool initialization and growth)
    static char* alloc_aligned_buffer();

    /**
     * @brief Initialize I/O buffer pool with pre-allocated aligned buffers
     */
    void init_io_buffer_pool(Size pool_size);
};

}  // namespace agent_mem_io