/**
 * @file memtable.cpp
 * @brief MemTable implementation
 */

#include "compaction/memtable.h"
#include <algorithm>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

// Suppress unused parameter warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace agent_mem_io {

// =============================================================================
// MemTable Implementation
// =============================================================================

MemTable::MemTable(const MemTableConfig& config)
    : config_(config)
{
    entries_.reserve(config_.max_entries);
}

MemTable::~MemTable() = default;

Error MemTable::insert(NodeId node_id, const Vector& vector) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (immutable_) {
        return Error::invalid_argument("Cannot insert into immutable MemTable");
    }
    
    if (entries_.size() >= config_.max_entries) {
        return Error::memory_limit_exceeded("MemTable is full");
    }
    
    Size entry_size = sizeof(MemTableEntry) + vector.size() * sizeof(float);
    if (current_size_ + entry_size > config_.max_size) {
        return Error::memory_limit_exceeded("MemTable size limit reached");
    }
    
    // Check if already exists
    auto it = index_.find(node_id);
    if (it != index_.end()) {
        // Update existing entry
        MemTableEntry& entry = entries_[it->second];
        Size old_size = sizeof(MemTableEntry) + entry.vector.size() * sizeof(float);
        current_size_ -= old_size;
        
        entry.vector = vector;
        entry.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        entry.is_deleted = false;
        
        current_size_ += entry_size;
    } else {
        // Insert new entry
        Size idx = entries_.size();
        entries_.emplace_back(node_id, vector, 
            std::chrono::system_clock::now().time_since_epoch().count());
        index_[node_id] = idx;
        current_size_ += entry_size;
    }
    
    return Error::success();
}

Error MemTable::delete_entry(NodeId node_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (immutable_) {
        return Error::invalid_argument("Cannot delete from immutable MemTable");
    }
    
    auto it = index_.find(node_id);
    if (it == index_.end()) {
        // Insert tombstone
        Size idx = entries_.size();
        entries_.emplace_back(node_id, Vector{}, 
            std::chrono::system_clock::now().time_since_epoch().count(), true);
        index_[node_id] = idx;
    } else {
        entries_[it->second].is_deleted = true;
    }
    
    return Error::success();
}

bool MemTable::get(NodeId node_id, Vector& vector) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = index_.find(node_id);
    if (it == index_.end()) {
        return false;
    }
    
    const MemTableEntry& entry = entries_[it->second];
    if (entry.is_deleted) {
        return false;
    }
    
    vector = entry.vector;
    return true;
}

bool MemTable::contains(NodeId node_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return index_.find(node_id) != index_.end();
}

bool MemTable::is_deleted(NodeId node_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = index_.find(node_id);
    if (it == index_.end()) {
        return false;
    }
    return entries_[it->second].is_deleted;
}

std::vector<MemTableEntry> MemTable::get_all_entries() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entries_;
}

std::vector<MemTableEntry> MemTable::get_entries_in_range(NodeId start_id, NodeId end_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<MemTableEntry> result;
    for (const auto& entry : entries_) {
        if (entry.node_id >= start_id && entry.node_id <= end_id) {
            result.push_back(entry);
        }
    }
    return result;
}

Size MemTable::get_size() const {
    return current_size_;
}

Size MemTable::get_num_entries() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entries_.size();
}

bool MemTable::is_full() const {
    return current_size_ >= config_.max_size || entries_.size() >= config_.max_entries;
}

void MemTable::mark_immutable() {
    immutable_ = true;
}

bool MemTable::is_immutable() const {
    return immutable_;
}

void MemTable::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_.clear();
    index_.clear();
    current_size_ = 0;
    immutable_ = false;
}

Size MemTable::get_memory_usage() const {
    return current_size_ + entries_.capacity() * sizeof(MemTableEntry) + 
           index_.size() * (sizeof(NodeId) + sizeof(Size));
}

// =============================================================================
// SSTableManager Implementation
// =============================================================================

SSTableManager::SSTableManager(const std::string& data_dir)
    : data_dir_(data_dir)
{
}

SSTableManager::~SSTableManager() = default;

Error SSTableManager::create_from_memtable(const MemTable& memtable, uint64_t& sstable_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto entries = memtable.get_all_entries();
    if (entries.empty()) {
        return Error::invalid_argument("MemTable is empty");
    }
    
    // Generate SSTable ID
    sstable_id = sstables_.size();
    
    // Create metadata
    SSTableMetadata meta;
    meta.id = sstable_id;
    meta.num_entries = entries.size();
    meta.creation_time = std::chrono::system_clock::now().time_since_epoch().count();
    meta.level = 0;
    
    // Find min/max node IDs
    meta.min_node_id = entries[0].node_id;
    meta.max_node_id = entries[0].node_id;
    for (const auto& entry : entries) {
        meta.min_node_id = std::min(meta.min_node_id, entry.node_id);
        meta.max_node_id = std::max(meta.max_node_id, entry.node_id);
    }
    
    // Create file path
    meta.filepath = data_dir_ + "/sstable_" + std::to_string(sstable_id) + ".bin";
    
    // Write to file
    Error err = write_sstable_file(meta.filepath, entries);
    if (!err.ok()) {
        return err;
    }
    
    // Calculate size
    meta.size_bytes = entries.size() * (sizeof(NodeId) + sizeof(uint32_t) + 
                                         DEFAULT_DIMENSION * sizeof(float) + sizeof(bool));
    
    // Store metadata
    sstables_[sstable_id] = meta;
    level_map_[0].push_back(sstable_id);
    
    return Error::success();
}

bool SSTableManager::read_vector(uint64_t sstable_id, NodeId node_id, Vector& vector) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = sstables_.find(sstable_id);
    if (it == sstables_.end()) {
        return false;
    }
    
    const SSTableMetadata& meta = it->second;
    if (node_id < meta.min_node_id || node_id > meta.max_node_id) {
        return false;
    }
    
    // TODO: Implement actual file reading
    return false;
}

Size SSTableManager::read_vectors_batch(uint64_t sstable_id, 
                                         const std::vector<NodeId>& node_ids,
                                         std::vector<Vector>& vectors) {
    // TODO: Implement batch reading
    return 0;
}

Error SSTableManager::delete_sstable(uint64_t sstable_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = sstables_.find(sstable_id);
    if (it == sstables_.end()) {
        return Error::node_not_found("SSTable not found: " + std::to_string(sstable_id));
    }
    
    // Delete file
    std::remove(it->second.filepath.c_str());
    
    // Remove from level map
    auto& level_list = level_map_[it->second.level];
    level_list.erase(std::remove(level_list.begin(), level_list.end(), sstable_id), 
                     level_list.end());
    
    // Remove metadata
    sstables_.erase(it);
    
    return Error::success();
}

SSTableMetadata SSTableManager::get_metadata(uint64_t sstable_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = sstables_.find(sstable_id);
    if (it == sstables_.end()) {
        return SSTableMetadata();
    }
    return it->second;
}

std::vector<SSTableMetadata> SSTableManager::get_all_metadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<SSTableMetadata> result;
    for (const auto& pair : sstables_) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<SSTableMetadata> SSTableManager::get_sstables_in_level(uint32_t level) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<SSTableMetadata> result;
    auto it = level_map_.find(level);
    if (it != level_map_.end()) {
        for (uint64_t id : it->second) {
            auto sst_it = sstables_.find(id);
            if (sst_it != sstables_.end()) {
                result.push_back(sst_it->second);
            }
        }
    }
    return result;
}

std::vector<SSTableMetadata> SSTableManager::get_sstables_containing(NodeId node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<SSTableMetadata> result;
    for (const auto& pair : sstables_) {
        if (node_id >= pair.second.min_node_id && node_id <= pair.second.max_node_id) {
            result.push_back(pair.second);
        }
    }
    return result;
}

Size SSTableManager::get_total_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Size total = 0;
    for (const auto& pair : sstables_) {
        total += pair.second.size_bytes;
    }
    return total;
}

Size SSTableManager::get_num_sstables() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sstables_.size();
}

Error SSTableManager::write_sstable_file(const std::string& filepath,
                                          const std::vector<MemTableEntry>& entries) {
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return Error::io_error("Failed to create SSTable file: " + filepath);
    }
    
    // Write header
    const char magic[] = "SSTABLE1";
    file.write(magic, 8);
    
    uint64_t num_entries = entries.size();
    file.write(reinterpret_cast<const char*>(&num_entries), sizeof(num_entries));
    
    // Write entries
    for (const auto& entry : entries) {
        file.write(reinterpret_cast<const char*>(&entry.node_id), sizeof(entry.node_id));
        
        uint32_t vec_size = static_cast<uint32_t>(entry.vector.size());
        file.write(reinterpret_cast<const char*>(&vec_size), sizeof(vec_size));
        
        if (!entry.vector.empty()) {
            file.write(reinterpret_cast<const char*>(entry.vector.data()),
                      entry.vector.size() * sizeof(float));
        }
        
        file.write(reinterpret_cast<const char*>(&entry.is_deleted), sizeof(entry.is_deleted));
    }
    
    file.close();
    return Error::success();
}

// =============================================================================
// CompactionManager Implementation
// =============================================================================

CompactionManager::CompactionManager(SSTableManager* sstable_manager,
                                       const CompactionConfig& config)
    : sstable_manager_(sstable_manager)
    , config_(config)
{
}

CompactionManager::~CompactionManager() {
    stop();
}

Error CompactionManager::start() {
    if (running_) {
        return Error::invalid_argument("Compaction manager already running");
    }
    
    running_ = true;
    compaction_thread_ = std::thread(&CompactionManager::compaction_thread_func, this);
    
    return Error::success();
}

void CompactionManager::stop() {
    running_ = false;
    cv_.notify_all();
    
    if (compaction_thread_.joinable()) {
        compaction_thread_.join();
    }
}

Error CompactionManager::trigger_compaction(uint32_t level) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_queue_.push(level);
    cv_.notify_one();
    return Error::success();
}

bool CompactionManager::needs_compaction(uint32_t level) const {
    if (!sstable_manager_) return false;
    
    auto sstables = sstable_manager_->get_sstables_in_level(level);
    return sstables.size() >= config_.min_sstables_to_compact;
}

bool CompactionManager::is_compacting() const {
    return compacting_;
}

Size CompactionManager::pending_compactions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_queue_.size();
}

void CompactionManager::set_io_bandwidth_limit(double limit) {
    io_bandwidth_limit_ = std::max(0.0, std::min(1.0, limit));
}

void CompactionManager::pause() {
    paused_ = true;
}

void CompactionManager::resume() {
    paused_ = false;
    cv_.notify_all();
}

void CompactionManager::register_callback(CompactionCallback callback) {
    callback_ = std::move(callback);
}

void CompactionManager::compaction_thread_func() {
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        cv_.wait_for(lock, std::chrono::milliseconds(config_.compaction_interval_ms), [this] {
            return !running_ || !pending_queue_.empty();
        });
        
        if (!running_) break;
        
        if (paused_ || pending_queue_.empty()) {
            continue;
        }
        
        uint32_t level = pending_queue_.front();
        pending_queue_.pop();
        
        lock.unlock();
        
        // Perform compaction
        compact_level(level);
    }
}

Error CompactionManager::compact_level(uint32_t level) {
    compacting_ = true;
    
    // TODO: Implement actual compaction
    
    compacting_ = false;
    
    if (callback_) {
        callback_(true, "Compaction completed for level " + std::to_string(level));
    }
    
    return Error::success();
}

Error CompactionManager::merge_sstables(const std::vector<uint64_t>& sstable_ids,
                                         uint64_t& output_id) {
    // TODO: Implement SSTable merging
    return Error::success();
}

bool CompactionManager::should_pause() const {
    return paused_;
}

// =============================================================================
// WalManager Implementation
// =============================================================================

WalManager::WalManager(const std::string& filepath)
    : filepath_(filepath)
    , fd_(-1)
{
}

WalManager::~WalManager() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Error WalManager::init() {
    fd_ = ::open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        return Error::io_error("Failed to open WAL file: " + filepath_);
    }
    return Error::success();
}

Error WalManager::log_insert(NodeId node_id, const Vector& vector) {
    MemTableEntry entry(node_id, vector, 
        std::chrono::system_clock::now().time_since_epoch().count(), false);
    return write_entry(entry);
}

Error WalManager::log_delete(NodeId node_id) {
    MemTableEntry entry(node_id, Vector{}, 
        std::chrono::system_clock::now().time_since_epoch().count(), true);
    return write_entry(entry);
}

Error WalManager::sync() {
    if (fd_ >= 0) {
        ::fsync(fd_);
    }
    return Error::success();
}

Error WalManager::recover(std::vector<MemTableEntry>& entries) {
    // TODO: Implement WAL recovery
    return Error::success();
}

Error WalManager::clear() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = ::open(filepath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        return Error::io_error("Failed to clear WAL file: " + filepath_);
    }
    current_size_ = 0;
    return Error::success();
}

Size WalManager::get_size() const {
    return current_size_;
}

Error WalManager::write_entry(const MemTableEntry& entry) {
    if (fd_ < 0) {
        return Error::io_error("WAL file not open");
    }
    
    // Write entry to WAL
    // TODO: Implement actual WAL writing
    current_size_ += sizeof(MemTableEntry) + entry.vector.size() * sizeof(float);
    
    return Error::success();
}

// =============================================================================
// LsmWriteManager Implementation
// =============================================================================

LsmWriteManager::LsmWriteManager(
    const std::string& data_dir,
    const MemTableConfig& memtable_config,
    const CompactionConfig& compaction_config
)
    : data_dir_(data_dir)
{
    active_memtable_ = std::make_unique<MemTable>(memtable_config);
    sstable_manager_ = std::make_unique<SSTableManager>(data_dir);
    compaction_manager_ = std::make_unique<CompactionManager>(sstable_manager_.get(), compaction_config);
    wal_manager_ = std::make_unique<WalManager>(data_dir + "/" + DEFAULT_WAL_FILE);
}

LsmWriteManager::~LsmWriteManager() {
    if (compaction_manager_) {
        compaction_manager_->stop();
    }
}

Error LsmWriteManager::init() {
    Error err = wal_manager_->init();
    if (!err.ok()) {
        return err;
    }
    
    err = compaction_manager_->start();
    if (!err.ok()) {
        return err;
    }
    
    return Error::success();
}

Error LsmWriteManager::insert(const Vector& vector, NodeId& node_id) {
    node_id = next_node_id_++;
    return insert_with_id(node_id, vector);
}

Error LsmWriteManager::insert_with_id(NodeId node_id, const Vector& vector) {
    // Write to WAL first
    Error err = wal_manager_->log_insert(node_id, vector);
    if (!err.ok()) {
        return err;
    }
    
    // Insert into MemTable
    err = active_memtable_->insert(node_id, vector);
    if (!err.ok()) {
        return err;
    }
    
    // Check if MemTable is full
    if (active_memtable_->is_full()) {
        err = rotate_memtable();
        if (!err.ok()) {
            return err;
        }
    }
    
    return Error::success();
}

Error LsmWriteManager::delete_entry(NodeId node_id) {
    Error err = wal_manager_->log_delete(node_id);
    if (!err.ok()) {
        return err;
    }
    
    return active_memtable_->delete_entry(node_id);
}

bool LsmWriteManager::get(NodeId node_id, Vector& vector) const {
    // Check active MemTable
    if (active_memtable_->get(node_id, vector)) {
        return true;
    }
    
    // Check immutable MemTables
    for (const auto& memtable : immutable_memtables_) {
        if (memtable->get(node_id, vector)) {
            return true;
        }
    }
    
    // TODO: Check SSTables
    
    return false;
}

Error LsmWriteManager::flush() {
    return rotate_memtable();
}

Size LsmWriteManager::get_memory_usage() const {
    Size total = active_memtable_->get_memory_usage();
    for (const auto& memtable : immutable_memtables_) {
        total += memtable->get_memory_usage();
    }
    return total;
}

Size LsmWriteManager::get_total_entries() const {
    Size total = active_memtable_->get_num_entries();
    for (const auto& memtable : immutable_memtables_) {
        total += memtable->get_num_entries();
    }
    return total;
}

Error LsmWriteManager::rotate_memtable() {
    // Mark current MemTable as immutable
    active_memtable_->mark_immutable();
    
    // Move to immutable list
    immutable_memtables_.push_back(std::move(active_memtable_));
    
    // Create new active MemTable with default config
    active_memtable_ = std::make_unique<MemTable>(MemTableConfig());
    
    // Flush oldest immutable MemTable
    if (!immutable_memtables_.empty()) {
        uint64_t sstable_id;
        Error err = sstable_manager_->create_from_memtable(*immutable_memtables_.front(), sstable_id);
        if (!err.ok()) {
            return err;
        }
        
        // Clear WAL after successful flush
        wal_manager_->clear();
        
        // Remove flushed MemTable
        immutable_memtables_.pop_front();
    }
    
    return Error::success();
}

}  // namespace agent_mem_io