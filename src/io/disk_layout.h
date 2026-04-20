/**
 * @file disk_layout.h
 * @brief Disk-resident node record layout for O_DIRECT SSD access
 *
 * Defines the on-disk format for vector + graph + PQ data, and provides
 * reader/writer classes with:
 *   - 4KB-aligned O_DIRECT I/O (SSD-friendly, bypasses OS Page Cache)
 *   - Batch reads via preadv (multiple nodes in fewer syscalls)
 *   - LRU vector cache (reduce SSD reads for hot nodes)
 *   - Pre-allocated buffer pool (eliminate per-read alloc/dealloc overhead)
 */

#pragma once

#include "common/types.h"
#include "core/pq_encoder.h"

#include <vector>
#include <list>
#include <unordered_map>
#include <string>
#include <cstring>
#include <algorithm>

namespace agent_mem_io {

// =============================================================================
// Constants
// =============================================================================

/// Disk record size: fixed 4KB for O_DIRECT alignment and SSD page size
constexpr Size DISK_RECORD_SIZE = 4096;

/// Maximum graph degree stored on disk (room for 32 neighbors)
constexpr Size DEFAULT_DISK_MAX_DEGREE = 32;

/// Header size: node_id (4B)
constexpr Size DISK_RECORD_HEADER_SIZE = sizeof(NodeId);

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
    float vector_data[DEFAULT_DIMENSION];  // 128 floats
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
// VectorCache - LRU cache for SSD-resident vectors
// =============================================================================

/**
 * @brief LRU cache for node vectors read from SSD
 *
 * Caches recently-accessed vectors to reduce SSD reads for hot nodes.
 * Memory usage is strictly controlled to fit within the 10-20% budget.
 *
 * Design: std::list (LRU order) + unordered_map (O(1) lookup)
 * Each entry stores the full DiskNodeRecord (vector + neighbor info),
 * so cached nodes can be fully processed without any SSD read.
 */
class VectorCache {
public:
    /**
     * @brief Construct vector cache with given capacity
     * @param capacity Maximum number of entries (0 = disabled)
     */
    explicit VectorCache(Size capacity = 0);

    /**
     * @brief Check if a node is cached
     * @param node_id Node ID
     * @return true if cached
     */
    bool has(NodeId node_id) const;

    /**
     * @brief Get cached node record (no SSD read needed)
     * @param node_id Node ID
     * @return Pointer to cached record, nullptr if not cached
     */
    const DiskNodeRecord* get(NodeId node_id);

    /**
     * @brief Put a node record into cache
     * @param node_id Node ID
     * @param record Disk node record to cache
     */
    void put(NodeId node_id, const DiskNodeRecord& record);

    /**
     * @brief Clear all cache entries
     */
    void clear();

    /**
     * @brief Get number of cached entries
     */
    Size size() const { return map_.size(); }

    /**
     * @brief Get maximum capacity
     */
    Size capacity() const { return capacity_; }

    /**
     * @brief Set new capacity (evicts entries if over new limit)
     */
    void set_capacity(Size new_capacity);

    /**
     * @brief Get cache memory usage in bytes
     * Each entry: DiskNodeRecord (~908 bytes) + hashmap overhead (~64 bytes)
     */
    Size memory_usage() const;

    /**
     * @brief Get cache hit count
     */
    uint64_t hit_count() const { return hits_; }

    /**
     * @brief Get cache miss count
     */
    uint64_t miss_count() const { return misses_; }

    /**
     * @brief Get cache hit rate
     */
    double hit_rate() const;

    /// Memory per entry (DiskNodeRecord struct + hashmap overhead estimate)
    static constexpr Size ENTRY_MEMORY = DiskNodeRecord::struct_size() + 64;

private:
    Size capacity_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;

    /// LRU list: front = most recently used, back = least recently used
    std::list<NodeId> lru_;

    /// Map: node_id -> (DiskNodeRecord, iterator into LRU list)
    std::unordered_map<NodeId,
        std::pair<DiskNodeRecord, std::list<NodeId>::iterator>> map_;

    void evict_if_over_capacity();
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
 *   - LRU vector cache (reduce SSD reads for hot/hub nodes)
 *   - Pre-allocated buffer pool (eliminate per-read alloc/dealloc overhead)
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

    // --- Batch reads (NEW) ---

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

    // --- Vector Cache (NEW) ---

    /**
     * @brief Configure vector cache capacity
     * @param max_entries Maximum number of records to cache (0 = disabled)
     */
    void set_cache_capacity(Size max_entries);

    /**
     * @brief Check if a node is cached
     */
    bool is_cached(NodeId node_id) const { return cache_.has(node_id); }

    /**
     * @brief Get cached record (no SSD read)
     * @return Pointer to cached record, nullptr if not cached
     */
    const DiskNodeRecord* get_cached_record(NodeId node_id) { return cache_.get(node_id); }

    /**
     * @brief Get cache hit rate
     */
    double get_cache_hit_rate() const { return cache_.hit_rate(); }

    /**
     * @brief Get cache memory usage
     */
    Size get_cache_memory_usage() const { return cache_.memory_usage(); }

    /**
     * @brief Get cache size (number of entries)
     */
    Size get_cache_size() const { return cache_.size(); }

    /**
     * @brief Get cache capacity
     */
    Size get_cache_capacity() const { return cache_.capacity(); }

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
     * @brief Get buffer pool size
     */
    Size get_buffer_pool_size() const { return buffer_pool_.size(); }

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

    /// LRU vector cache
    VectorCache cache_;

    /// Pre-allocated buffer pool for O_DIRECT reads
    std::vector<char*> buffer_pool_;
    Size buffer_pool_capacity_ = 64;  // Default: 64 pre-allocated buffers

    /// Allocate a new aligned buffer (for pool initialization and growth)
    static char* alloc_aligned_buffer();

    /**
     * @brief Initialize buffer pool with pre-allocated aligned buffers
     */
    void init_buffer_pool(Size pool_size);
};

}  // namespace agent_mem_io