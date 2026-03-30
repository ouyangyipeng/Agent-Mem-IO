/**
 * @file memtable.h
 * @brief MemTable and SSTable for LSM-Tree style write optimization
 * 
 * This file implements the write path components for the storage engine,
 * following LSM-Tree design principles to convert random writes into
 * sequential writes and manage background compaction.
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <deque>
#include <functional>
#include <chrono>

namespace agent_mem_io {

/**
 * @brief MemTable entry structure
 */
struct MemTableEntry {
    NodeId node_id;           // Node identifier
    Vector vector;            // Vector data
    uint64_t timestamp;       // Insertion timestamp
    bool is_deleted;          // Deletion marker (for tombstones)
    
    MemTableEntry()
        : node_id(INVALID_NODE_ID)
        , timestamp(0)
        , is_deleted(false)
    {}
    
    MemTableEntry(NodeId id, const Vector& vec, uint64_t ts, bool del = false)
        : node_id(id)
        , vector(vec)
        , timestamp(ts)
        , is_deleted(del)
    {}
};

/**
 * @brief MemTable configuration
 */
struct MemTableConfig {
    Size max_size = DEFAULT_MEMTABLE_SIZE;  // Maximum size in bytes
    Size max_entries = 100000;               // Maximum number of entries
    bool enable_wal = true;                  // Enable write-ahead logging
    
    MemTableConfig() = default;
};

/**
 * @brief Active MemTable for real-time writes
 * 
 * The MemTable is an in-memory sorted structure that buffers incoming writes.
 * When it reaches the size limit, it becomes immutable and a new active
 * MemTable is created. Immutable MemTables are flushed to SSTables on SSD.
 */
class MemTable {
public:
    /**
     * @brief Construct MemTable
     * @param config MemTable configuration
     */
    explicit MemTable(const MemTableConfig& config);
    
    /**
     * @brief Destructor
     */
    ~MemTable();
    
    /**
     * @brief Insert a vector
     * @param node_id Node identifier
     * @param vector Vector data
     * @return Error status
     */
    Error insert(NodeId node_id, const Vector& vector);
    
    /**
     * @brief Delete a vector (mark as tombstone)
     * @param node_id Node identifier
     * @return Error status
     */
    Error delete_entry(NodeId node_id);
    
    /**
     * @brief Get a vector by node ID
     * @param node_id Node identifier
     * @param vector Output vector
     * @return true if found, false otherwise
     */
    bool get(NodeId node_id, Vector& vector) const;
    
    /**
     * @brief Check if entry exists
     * @param node_id Node identifier
     * @return true if exists (including tombstones)
     */
    bool contains(NodeId node_id) const;
    
    /**
     * @brief Check if entry is deleted (tombstone)
     * @param node_id Node identifier
     * @return true if marked as deleted
     */
    bool is_deleted(NodeId node_id) const;
    
    /**
     * @brief Get all entries
     * @return List of all entries
     */
    std::vector<MemTableEntry> get_all_entries() const;
    
    /**
     * @brief Get entries in range
     * @param start_id Start node ID
     * @param end_id End node ID
     * @return List of entries in range
     */
    std::vector<MemTableEntry> get_entries_in_range(NodeId start_id, NodeId end_id) const;
    
    /**
     * @brief Get current size in bytes
     * @return Size in bytes
     */
    Size get_size() const;
    
    /**
     * @brief Get number of entries
     * @return Number of entries
     */
    Size get_num_entries() const;
    
    /**
     * @brief Check if MemTable is full
     * @return true if size limit reached
     */
    bool is_full() const;
    
    /**
     * @brief Mark MemTable as immutable
     */
    void mark_immutable();
    
    /**
     * @brief Check if MemTable is immutable
     * @return true if immutable
     */
    bool is_immutable() const;
    
    /**
     * @brief Clear MemTable
     */
    void clear();
    
    /**
     * @brief Get memory usage
     * @return Memory usage in bytes
     */
    Size get_memory_usage() const;

private:
    MemTableConfig config_;
    
    // Entries sorted by node_id
    std::vector<MemTableEntry> entries_;
    
    // Index for fast lookup: node_id -> entry index
    std::unordered_map<NodeId, Size> index_;
    
    // Current size in bytes
    std::atomic<Size> current_size_{0};
    
    // Immutable flag
    std::atomic<bool> immutable_{false};
    
    // Mutex for thread-safe access
    mutable std::shared_mutex mutex_;
};

/**
 * @brief SSTable (Sorted String Table) on SSD
 * 
 * SSTable is the on-disk representation of a flushed MemTable.
 * It stores vectors in sorted order by node_id for efficient retrieval.
 */
struct SSTableMetadata {
    uint64_t id;              // SSTable ID (unique)
    NodeId min_node_id;       // Minimum node ID in table
    NodeId max_node_id;       // Maximum node ID in table
    Size num_entries;         // Number of entries
    Size size_bytes;          // Size in bytes
    uint64_t creation_time;   // Creation timestamp
    std::string filepath;     // File path on SSD
    uint32_t level;           // Level in LSM tree
    
    SSTableMetadata()
        : id(0)
        , min_node_id(INVALID_NODE_ID)
        , max_node_id(INVALID_NODE_ID)
        , num_entries(0)
        , size_bytes(0)
        , creation_time(0)
        , level(0)
    {}
};

/**
 * @brief SSTable manager
 * 
 * Manages SSTable files on SSD, including creation, reading, and deletion.
 */
class SSTableManager {
public:
    /**
     * @brief Construct SSTable manager
     * @param data_dir Data directory for SSTable files
     */
    explicit SSTableManager(const std::string& data_dir);
    
    /**
     * @brief Destructor
     */
    ~SSTableManager();
    
    /**
     * @brief Create SSTable from MemTable
     * @param memtable MemTable to flush
     * @param sstable_id Output SSTable ID
     * @return Error status
     */
    Error create_from_memtable(const MemTable& memtable, uint64_t& sstable_id);
    
    /**
     * @brief Read vector from SSTable
     * @param sstable_id SSTable ID
     * @param node_id Node identifier
     * @param vector Output vector
     * @return true if found, false otherwise
     */
    bool read_vector(uint64_t sstable_id, NodeId node_id, Vector& vector);
    
    /**
     * @brief Read multiple vectors from SSTable
     * @param sstable_id SSTable ID
     * @param node_ids List of node IDs
     * @param vectors Output vectors
     * @return Number of vectors read
     */
    Size read_vectors_batch(uint64_t sstable_id, const std::vector<NodeId>& node_ids, 
                            std::vector<Vector>& vectors);
    
    /**
     * @brief Delete SSTable
     * @param sstable_id SSTable ID
     * @return Error status
     */
    Error delete_sstable(uint64_t sstable_id);
    
    /**
     * @brief Get SSTable metadata
     * @param sstable_id SSTable ID
     * @return Metadata, or empty if not found
     */
    SSTableMetadata get_metadata(uint64_t sstable_id) const;
    
    /**
     * @brief Get all SSTable metadata
     * @return List of all SSTable metadata
     */
    std::vector<SSTableMetadata> get_all_metadata() const;
    
    /**
     * @brief Get SSTables in level
     * @param level Level number
     * @return List of SSTable metadata in level
     */
    std::vector<SSTableMetadata> get_sstables_in_level(uint32_t level) const;
    
    /**
     * @brief Get SSTables containing node ID
     * @param node_id Node identifier
     * @return List of SSTable metadata containing the node
     */
    std::vector<SSTableMetadata> get_sstables_containing(NodeId node_id) const;
    
    /**
     * @brief Get total size of all SSTables
     * @return Total size in bytes
     */
    Size get_total_size() const;
    
    /**
     * @brief Get number of SSTables
     * @return Number of SSTables
     */
    Size get_num_sstables() const;

private:
    /**
     * @brief Write SSTable file
     * @param filepath File path
     * @param entries Entries to write
     * @return Error status
     */
    Error write_sstable_file(const std::string& filepath, 
                             const std::vector<MemTableEntry>& entries);
    
    /**
     * @brief Read SSTable file header
     * @param fd File descriptor
     * @param header Output header
     * @return Error status
     */
    Error read_sstable_header(int fd, SSTableMetadata& header);
    
    std::string data_dir_;
    
    // SSTable metadata map: id -> metadata
    std::unordered_map<uint64_t, SSTableMetadata> sstables_;
    
    // Level map: level -> list of SSTable IDs
    std::unordered_map<uint32_t, std::vector<uint64_t>> level_map_;
    
    mutable std::mutex mutex_;
};

/**
 * @brief Compaction configuration
 */
struct CompactionConfig {
    uint32_t max_levels = 7;              // Maximum levels in LSM tree
    Size level_size_ratio = 10;           // Size ratio between levels
    Size min_sstables_to_compact = 2;     // Minimum SSTables to trigger compaction
    uint32_t compaction_threads = 2;      // Number of compaction threads
    bool enable_background_compaction = true;  // Enable background compaction
    uint32_t compaction_interval_ms = 1000;    // Compaction check interval
    double io_bandwidth_limit = 0.5;      // I/O bandwidth limit (fraction of total)
    
    CompactionConfig() = default;
};

/**
 * @brief Compaction manager
 * 
 * Manages background compaction of SSTables, merging multiple SSTables
 * into larger ones and rebuilding graph index to maintain connectivity.
 */
class CompactionManager {
public:
    /**
     * @brief Construct compaction manager
     * @param sstable_manager SSTable manager
     * @param config Compaction configuration
     */
    CompactionManager(SSTableManager* sstable_manager, 
                      const CompactionConfig& config);
    
    /**
     * @brief Destructor
     */
    ~CompactionManager();
    
    /**
     * @brief Start background compaction thread
     * @return Error status
     */
    Error start();
    
    /**
     * @brief Stop background compaction thread
     */
    void stop();
    
    /**
     * @brief Trigger manual compaction
     * @param level Level to compact
     * @return Error status
     */
    Error trigger_compaction(uint32_t level);
    
    /**
     * @brief Check if compaction is needed
     * @param level Level to check
     * @return true if compaction is needed
     */
    bool needs_compaction(uint32_t level) const;
    
    /**
     * @brief Get compaction status
     * @return true if compaction is running
     */
    bool is_compacting() const;
    
    /**
     * @brief Get number of pending compactions
     * @return Number of pending compactions
     */
    Size pending_compactions() const;
    
    /**
     * @brief Set I/O bandwidth limit
     * @param limit Bandwidth limit (fraction of total, 0.0 to 1.0)
     */
    void set_io_bandwidth_limit(double limit);
    
    /**
     * @brief Pause compaction (for query priority)
     */
    void pause();
    
    /**
     * @brief Resume compaction
     */
    void resume();
    
    /**
     * @brief Register compaction completion callback
     * @param callback Callback function
     */
    void register_callback(CompactionCallback callback);

private:
    /**
     * @brief Background compaction thread function
     */
    void compaction_thread_func();
    
    /**
     * @brief Compact SSTables in a level
     * @param level Level to compact
     * @return Error status
     */
    Error compact_level(uint32_t level);
    
    /**
     * @brief Merge multiple SSTables
     * @param sstable_ids SSTable IDs to merge
     * @param output_id Output SSTable ID
     * @return Error status
     */
    Error merge_sstables(const std::vector<uint64_t>& sstable_ids, 
                         uint64_t& output_id);
    
    /**
     * @brief Check if should pause for query priority
     * @return true if should pause
     */
    bool should_pause() const;
    
    SSTableManager* sstable_manager_;
    CompactionConfig config_;
    
    // Compaction thread
    std::thread compaction_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> compacting_{false};
    
    // Pending compaction queue
    std::queue<uint32_t> pending_queue_;
    
    // Callback
    CompactionCallback callback_;
    
    // I/O bandwidth limit
    std::atomic<double> io_bandwidth_limit_{0.5};
    
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

/**
 * @brief Write-Ahead Log (WAL) for durability
 * 
 * WAL ensures that writes are durable even if the system crashes.
 * All writes are first logged to WAL before being applied to MemTable.
 */
class WalManager {
public:
    /**
     * @brief Construct WAL manager
     * @param filepath WAL file path
     */
    explicit WalManager(const std::string& filepath);
    
    /**
     * @brief Destructor
     */
    ~WalManager();
    
    /**
     * @brief Initialize WAL
     * @return Error status
     */
    Error init();
    
    /**
     * @brief Log insert operation
     * @param node_id Node identifier
     * @param vector Vector data
     * @return Error status
     */
    Error log_insert(NodeId node_id, const Vector& vector);
    
    /**
     * @brief Log delete operation
     * @param node_id Node identifier
     * @return Error status
     */
    Error log_delete(NodeId node_id);
    
    /**
     * @brief Sync WAL to disk
     * @return Error status
     */
    Error sync();
    
    /**
     * @brief Recover from WAL
     * @param entries Output entries recovered
     * @return Error status
     */
    Error recover(std::vector<MemTableEntry>& entries);
    
    /**
     * @brief Clear WAL (after successful flush)
     * @return Error status
     */
    Error clear();
    
    /**
     * @brief Get WAL size
     * @return Size in bytes
     */
    Size get_size() const;

private:
    /**
     * @brief Write entry to WAL
     * @param entry Entry to write
     * @return Error status
     */
    Error write_entry(const MemTableEntry& entry);
    
    std::string filepath_;
    int fd_;
    std::atomic<Size> current_size_{0};
    
    mutable std::mutex mutex_;
};

/**
 * @brief LSM-Tree write manager
 * 
 * Coordinates MemTable, SSTable, WAL, and Compaction for the write path.
 */
class LsmWriteManager {
public:
    /**
     * @brief Construct LSM write manager
     * @param data_dir Data directory
     * @param memtable_config MemTable configuration
     * @param compaction_config Compaction configuration
     */
    LsmWriteManager(
        const std::string& data_dir,
        const MemTableConfig& memtable_config = MemTableConfig(),
        const CompactionConfig& compaction_config = CompactionConfig()
    );
    
    /**
     * @brief Destructor
     */
    ~LsmWriteManager();
    
    /**
     * @brief Initialize manager
     * @return Error status
     */
    Error init();
    
    /**
     * @brief Insert a vector
     * @param vector Vector data
     * @param node_id Output assigned node ID
     * @return Error status
     */
    Error insert(const Vector& vector, NodeId& node_id);
    
    /**
     * @brief Insert a vector with specific node ID
     * @param node_id Node identifier
     * @param vector Vector data
     * @return Error status
     */
    Error insert_with_id(NodeId node_id, const Vector& vector);
    
    /**
     * @brief Delete a vector
     * @param node_id Node identifier
     * @return Error status
     */
    Error delete_entry(NodeId node_id);
    
    /**
     * @brief Get a vector
     * @param node_id Node identifier
     * @param vector Output vector
     * @return true if found, false otherwise
     */
    bool get(NodeId node_id, Vector& vector) const;
    
    /**
     * @brief Flush active MemTable to SSTable
     * @return Error status
     */
    Error flush();
    
    /**
     * @brief Get memory usage
     * @return Memory usage in bytes
     */
    Size get_memory_usage() const;
    
    /**
     * @brief Get number of entries
     * @return Total number of entries
     */
    Size get_total_entries() const;

private:
    /**
     * @brief Rotate MemTable (active -> immutable)
     * @return Error status
     */
    Error rotate_memtable();
    
    std::string data_dir_;
    
    // Active MemTable
    std::unique_ptr<MemTable> active_memtable_;
    
    // Immutable MemTables (waiting for flush)
    std::deque<std::unique_ptr<MemTable>> immutable_memtables_;
    
    // SSTable manager
    std::unique_ptr<SSTableManager> sstable_manager_;
    
    // Compaction manager
    std::unique_ptr<CompactionManager> compaction_manager_;
    
    // WAL manager
    std::unique_ptr<WalManager> wal_manager_;
    
    // Next node ID
    std::atomic<NodeId> next_node_id_{0};
    
    mutable std::mutex mutex_;
};

}  // namespace agent_mem_io