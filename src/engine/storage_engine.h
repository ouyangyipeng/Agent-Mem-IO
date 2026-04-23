/**
 * @file storage_engine.h
 * @brief Main storage engine class integrating all components
 * 
 * This file defines the main storage engine that coordinates:
 * - Graph index (Vamana)
 * - Buffer pool (user-space cache)
 * - I/O engine (io_uring)
 * - Prefetcher (topology-aware)
 * - Write manager (LSM-Tree)
 */

#pragma once

#include "common/types.h"
#include "core/graph_index.h"
#include "buffer/buffer_pool.h"
#include "io/io_uring_engine.h"
#include "compaction/memtable.h"
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

namespace agent_mem_io {

// Forward declaration — DiskIndexReader is only used as pointer in QueryProcessor
class DiskIndexReader;

/**
 * @brief Storage engine configuration
 */
struct StorageEngineConfig {
    // Graph index configuration
    GraphIndexConfig graph_config;
    
    // Buffer pool configuration
    BufferPoolConfig buffer_pool_config;
    
    // I/O engine configuration
    IoEngineConfig io_engine_config;
    
    // MemTable configuration
    MemTableConfig memtable_config;
    
    // Compaction configuration
    CompactionConfig compaction_config;
    
    // General settings
    std::string data_dir = "./data";       // Data directory
    Dimension dimension = DEFAULT_DIMENSION; // Vector dimension
    Size memory_limit = 0;                  // Memory limit (0 = auto)
    bool enable_prefetch = true;            // Enable topology-aware prefetch
    bool enable_metrics = true;             // Enable performance metrics
    
    StorageEngineConfig() = default;
};

/**
 * @brief Topology-aware prefetcher
 * 
 * Implements graph topology-aware prefetching to hide I/O latency.
 * When visiting a node during search, it asynchronously prefetches
 * the node's neighbors' vector data from SSD.
 */
class TopologyAwarePrefetcher {
public:
    /**
     * @brief Construct prefetcher
     * @param io_engine I/O engine
     * @param buffer_pool Buffer pool
     * @param graph_index Graph index
     */
    TopologyAwarePrefetcher(
        IoEngine* io_engine,
        BufferPoolManager* buffer_pool,
        const GraphIndex* graph_index
    );
    
    /**
     * @brief Prefetch neighbors of a node
     * @param node_id Current node being visited
     * @param vector_fd Vector file descriptor
     * @return Number of prefetch requests submitted
     */
    Size prefetch_neighbors(NodeId node_id, int vector_fd);
    
    /**
     * @brief Wait for prefetch completions
     * @return Number of completions received
     */
    Size wait_prefetch_completions();
    
    /**
     * @brief Get prefetch statistics
     * @param total_prefetches Total prefetch requests
     * @param successful_prefetches Successful prefetches
     * @param cache_hits Prefetches that hit cache
     */
    void get_stats(uint64_t& total_prefetches, uint64_t& successful_prefetches, 
                   uint64_t& cache_hits) const;
    
    /**
     * @brief Reset statistics
     */
    void reset_stats();
    
    /**
     * @brief Set prefetch batch size
     * @param batch_size Maximum batch size
     */
    void set_batch_size(Size batch_size);

private:
    IoEngine* io_engine_;
    BufferPoolManager* buffer_pool_;
    const GraphIndex* graph_index_;
    
    Size batch_size_;
    
    // Statistics
    std::atomic<uint64_t> total_prefetches_{0};
    std::atomic<uint64_t> successful_prefetches_{0};
    std::atomic<uint64_t> cache_hits_{0};
};

/**
 * @brief Query processor
 *
 * Handles search queries using graph traversal with I/O optimization.
 * Optimized with VisitedBitmap, SIMD distance, ef_search control,
 * and compute_distance_direct() for zero-memcpy SSD reads.
 */
class QueryProcessor {
public:
    /**
     * @brief Construct query processor
     * @param graph_index Graph index
     * @param buffer_pool Buffer pool
     * @param io_engine I/O engine
     * @param prefetcher Prefetcher
     * @param vector_fd Vector file descriptor
     * @param disk_reader Optional DiskIndexReader for compute_distance_direct
     */
    QueryProcessor(
        const GraphIndex* graph_index,
        BufferPoolManager* buffer_pool,
        IoEngine* io_engine,
        TopologyAwarePrefetcher* prefetcher,
        int vector_fd,
        DiskIndexReader* disk_reader = nullptr
    );
    
    /**
     * @brief Search for top-K nearest neighbors
     * @param query Query vector
     * @param k Number of neighbors to return
     * @param results Output search results
     * @return Error status
     */
    Error search(const Vector& query, Size k, SearchResults& results);
    
    /**
     * @brief Search with beam width and ef_search control
     * @param query Query vector
     * @param k Number of neighbors
     * @param beam_width Beam width for search
     * @param ef_search Search beam width (controls recall/latency tradeoff)
     * @param results Output results
     * @return Error status
     */
    Error search_with_beam_width(
        const Vector& query,
        Size k,
        Size beam_width,
        Size ef_search,
        SearchResults& results
    );
    
    /**
     * @brief Get search statistics
     * @param total_searches Total searches
     * @param total_io_count Total I/O operations
     * @param avg_latency_ms Average latency
     */
    void get_stats(uint64_t& total_searches, uint64_t& total_io_count,
                   double& avg_latency_ms) const;
    
    /**
     * @brief Reset statistics
     */
    void reset_stats();

private:
    /**
     * @brief Get vector for a node
     * @param node_id Node identifier
     * @param vector Output vector
     * @return Error status
     */
    Error get_vector(NodeId node_id, Vector& vector);
    
    /**
     * @brief Calculate distance between query and node
     * @param query Query vector
     * @param node_id Node identifier
     * @return Distance
     */
    Distance calculate_distance(const Vector& query, NodeId node_id);
    
    const GraphIndex* graph_index_;
    BufferPoolManager* buffer_pool_;
    IoEngine* io_engine_;
    TopologyAwarePrefetcher* prefetcher_;
    DiskIndexReader* disk_reader_;
    int vector_fd_;
    
    // Statistics
    std::atomic<uint64_t> total_searches_{0};
    std::atomic<uint64_t> total_io_count_{0};
    std::atomic<uint64_t> total_latency_ns_{0};
};

/**
 * @brief Performance metrics collector
 */
struct PerformanceMetrics {
    // Query metrics
    uint64_t total_queries = 0;
    uint64_t total_query_time_ns = 0;
    double avg_query_latency_ms = 0.0;
    double p99_query_latency_ms = 0.0;
    double p999_query_latency_ms = 0.0;
    
    // I/O metrics
    uint64_t total_io_reads = 0;
    uint64_t total_io_writes = 0;
    double cache_hit_rate = 0.0;
    uint64_t prefetch_count = 0;
    uint64_t prefetch_hits = 0;
    
    // Write metrics
    uint64_t total_inserts = 0;
    uint64_t total_deletes = 0;
    uint64_t memtable_flushes = 0;
    uint64_t compaction_count = 0;
    
    // Memory metrics
    Size current_memory_usage = 0;
    Size peak_memory_usage = 0;
    double memory_limit_ratio = 0.0;
    
    // Recall metrics
    double recall_at_10 = 0.0;
    double recall_at_100 = 0.0;
    
    /**
     * @brief Calculate average query latency
     */
    void calculate_avg_latency();
    
    /**
     * @brief Reset all metrics
     */
    void reset();
};

/**
 * @brief Main storage engine class
 * 
 * Coordinates all components for vector storage and retrieval.
 */
class StorageEngine {
public:
    /**
     * @brief Construct storage engine
     * @param config Engine configuration
     */
    explicit StorageEngine(const StorageEngineConfig& config);
    
    /**
     * @brief Destructor
     */
    ~StorageEngine();
    
    /**
     * @brief Initialize the engine
     * @return Error status
     */
    Error init();
    
    /**
     * @brief Shutdown the engine
     */
    void shutdown();
    
    /**
     * @brief Load dataset from file
     * @param filepath Dataset file path
     * @param num_vectors Number of vectors to load
     * @return Error status
     */
    Error load_dataset(const std::string& filepath, Size num_vectors);
    
    /**
     * @brief Build index from loaded vectors
     * @return Error status
     */
    Error build_index();
    
    /**
     * @brief Insert a vector
     * @param vector Vector to insert
     * @param node_id Output assigned node ID
     * @return Error status
     */
    Error insert(const Vector& vector, NodeId& node_id);
    
    /**
     * @brief Insert a vector with specific ID
     * @param node_id Node identifier
     * @param vector Vector to insert
     * @return Error status
     */
    Error insert_with_id(NodeId node_id, const Vector& vector);
    
    /**
     * @brief Delete a vector
     * @param node_id Node identifier
     * @return Error status
     */
    Error delete_vector(NodeId node_id);
    
    /**
     * @brief Search for top-K nearest neighbors
     * @param query Query vector
     * @param k Number of neighbors
     * @param results Output search results
     * @return Error status
     */
    Error search(const Vector& query, Size k, SearchResults& results);
    
    /**
     * @brief Search with detailed result
     * @param query Query vector
     * @param k Number of neighbors
     * @param result Output detailed query result
     * @return Error status
     */
    Error search_with_details(const Vector& query, Size k, QueryResult& result);
    
    /**
     * @brief Batch search
     * @param queries List of query vectors
     * @param k Number of neighbors per query
     * @param results Output search results for each query
     * @return Error status
     */
    Error batch_search(
        const std::vector<Vector>& queries,
        Size k,
        std::vector<SearchResults>& results
    );
    
    /**
     * @brief Get number of vectors
     * @return Number of vectors in the engine
     */
    Size get_num_vectors() const;
    
    /**
     * @brief Get memory usage
     * @return Current memory usage in bytes
     */
    Size get_memory_usage() const;
    
    /**
     * @brief Check if memory is within limits
     * @return true if within limits
     */
    bool is_memory_within_limits() const;
    
    /**
     * @brief Get performance metrics
     * @return Current performance metrics
     */
    PerformanceMetrics get_metrics() const;
    
    /**
     * @brief Reset performance metrics
     */
    void reset_metrics();
    
    /**
     * @brief Save engine state to disk
     * @return Error status
     */
    Error save();
    
    /**
     * @brief Load engine state from disk
     * @return Error status
     */
    Error load();
    
    /**
     * @brief Check if engine is initialized
     * @return true if initialized
     */
    bool is_initialized() const;
    
    /**
     * @brief Get configuration
     * @return Current configuration
     */
    const StorageEngineConfig& get_config() const;
    
    /**
     * @brief Flush all pending writes
     * @return Error status
     */
    Error flush();
    
    /**
     * @brief Compact SSTables
     * @return Error status
     */
    Error compact();

private:
    /**
     * @brief Open vector file with O_DIRECT
     * @return Error status
     */
    Error open_vector_file();
    
    /**
     * @brief Close vector file
     */
    void close_vector_file();
    
    /**
     * @brief Update metrics after search
     * @param latency_ns Search latency in nanoseconds
     * @param io_count Number of I/O operations
     */
    void update_search_metrics(uint64_t latency_ns, uint64_t io_count);
    
    /**
     * @brief Update metrics after insert
     */
    void update_insert_metrics();
    
    StorageEngineConfig config_;
    
    // Components
    std::unique_ptr<GraphIndex> graph_index_;
    std::unique_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<IoEngine> io_engine_;
    std::unique_ptr<TopologyAwarePrefetcher> prefetcher_;
    std::unique_ptr<QueryProcessor> query_processor_;
    std::unique_ptr<LsmWriteManager> write_manager_;
    
    // File descriptors
    int vector_fd_;
    int graph_fd_;
    
    // State
    std::atomic<bool> initialized_{false};
    std::atomic<Size> num_vectors_{0};
    
    // Metrics
    PerformanceMetrics metrics_;
    
    // Latency samples for P99/P999 calculation
    std::vector<uint64_t> latency_samples_;
    
    mutable std::mutex mutex_;
};

/**
 * @brief Create storage engine with default configuration
 * @param data_dir Data directory
 * @param memory_limit Memory limit in bytes (0 = auto)
 * @return Storage engine instance
 */
std::unique_ptr<StorageEngine> create_storage_engine(
    const std::string& data_dir = "./data",
    Size memory_limit = 0
);

/**
 * @brief Create storage engine for SIFT1M dataset
 * @param data_dir Data directory
 * @return Storage engine instance configured for SIFT1M
 */
std::unique_ptr<StorageEngine> create_sift1m_engine(const std::string& data_dir = "./data");

}  // namespace agent_mem_io