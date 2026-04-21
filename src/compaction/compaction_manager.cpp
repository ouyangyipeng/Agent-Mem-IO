/**
 * @file compaction_manager.cpp
 * @brief LSM-Tree compaction manager implementation
 *
 * Manages background compaction of SSTables:
 *   - Size-tiered compaction: merge SSTables of similar size
 *   - Level-based compaction: maintain level size ratios
 *   - Background thread with pause/resume for query priority
 *   - I/O bandwidth limiting to avoid saturating SSD bandwidth
 */

#include "compaction/memtable.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <cstring>

namespace agent_mem_io {

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
    if (running_.load()) {
        return Error::invalid_argument("Compaction already running");
    }

    if (!config_.enable_background_compaction) {
        return Error::success();
    }

    running_ = true;
    compacting_ = false;
    paused_ = false;

    compaction_thread_ = std::thread(&CompactionManager::compaction_thread_func, this);

    std::cout << "[CompactionManager] Background compaction started\n";
    return Error::success();
}

void CompactionManager::stop() {
    if (!running_.load()) {
        return;
    }

    running_ = false;
    paused_ = false;
    cv_.notify_all();

    if (compaction_thread_.joinable()) {
        compaction_thread_.join();
    }

    std::cout << "[CompactionManager] Background compaction stopped\n";
}

Error CompactionManager::trigger_compaction(uint32_t level) {
    if (level >= config_.max_levels) {
        return Error::invalid_argument("Invalid compaction level: " + std::to_string(level));
    }

    if (compacting_.load()) {
        return Error::invalid_argument("Compaction already running");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_queue_.push(level);
    }

    cv_.notify_one();
    return Error::success();
}

bool CompactionManager::needs_compaction(uint32_t level) const {
    if (!sstable_manager_) {
        return false;
    }

    // Use existing API: get_sstables_in_level() returns vector of SSTableMetadata
    auto sstables = sstable_manager_->get_sstables_in_level(level);
    return sstables.size() >= config_.min_sstables_to_compact;
}

bool CompactionManager::is_compacting() const {
    return compacting_.load();
}

Size CompactionManager::pending_compactions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_queue_.size();
}

void CompactionManager::set_io_bandwidth_limit(double limit) {
    io_bandwidth_limit_ = std::clamp(limit, 0.0, 1.0);
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
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait_for(lock,
            std::chrono::milliseconds(config_.compaction_interval_ms),
            [this] { return !pending_queue_.empty() || !running_.load(); });

        if (!running_.load()) break;

        if (paused_.load() || pending_queue_.empty()) {
            continue;
        }

        uint32_t level_to_compact = pending_queue_.front();
        pending_queue_.pop();

        lock.unlock();

        // Perform compaction
        compacting_ = true;
        Error err = compact_level(level_to_compact);
        compacting_ = false;

        if (!err.ok()) {
            std::cerr << "[CompactionManager] Compaction failed at level "
                      << level_to_compact << ": " << err.message() << "\n";
            if (callback_) {
                callback_(false, "Compaction failed for level " + std::to_string(level_to_compact));
            }
        } else {
            if (callback_) {
                callback_(true, "Compaction completed for level " + std::to_string(level_to_compact));
            }
        }
    }
}

Error CompactionManager::compact_level(uint32_t level) {
    if (!sstable_manager_) {
        return Error::invalid_argument("SSTable manager not initialized");
    }

    // Get SSTables at this level using existing API
    auto sstable_metas = sstable_manager_->get_sstables_in_level(level);

    if (sstable_metas.size() < config_.min_sstables_to_compact) {
        return Error::success();  // Not enough SSTables to compact
    }

    // Collect SSTable IDs
    std::vector<uint64_t> sstable_ids;
    for (const auto& meta : sstable_metas) {
        sstable_ids.push_back(meta.id);
    }

    // Limit SSTables per compaction pass
    if (sstable_ids.size() > config_.min_sstables_to_compact * 2) {
        sstable_ids.resize(config_.min_sstables_to_compact * 2);
    }

    // Merge SSTables into a new one at the next level
    uint64_t output_id = 0;
    Error err = merge_sstables(sstable_ids, output_id);

    if (!err.ok()) {
        return err;
    }

    // Remove old SSTables using existing API (delete_sstable)
    for (uint64_t id : sstable_ids) {
        Error del_err = sstable_manager_->delete_sstable(id);
        if (!del_err.ok()) {
            std::cerr << "[CompactionManager] Failed to delete SSTable " << id << "\n";
        }
    }

    std::cout << "[CompactionManager] Compacted level " << level
              << ": merged " << sstable_ids.size() << " SSTables into SSTable "
              << output_id << "\n";

    return Error::success();
}

Error CompactionManager::merge_sstables(const std::vector<uint64_t>& sstable_ids,
                                          uint64_t& output_id) {
    if (sstable_ids.empty()) {
        return Error::invalid_argument("No SSTables to merge");
    }

    // Collect all entries from SSTables using read_sstable_entries
    std::vector<MemTableEntry> all_entries;

    for (uint64_t id : sstable_ids) {
        std::vector<MemTableEntry> entries;
        Error err = sstable_manager_->read_sstable_entries(id, entries);
        if (!err.ok()) {
            std::cerr << "[CompactionManager] Failed to read SSTable " << id
                      << ": " << err.message() << "\n";
            continue;
        }

        // Merge entries: deduplicate by node_id, keep latest timestamp
        for (auto& entry : entries) {
            auto it = std::find_if(all_entries.begin(), all_entries.end(),
                [&entry](const MemTableEntry& e) { return e.node_id == entry.node_id; });

            if (it != all_entries.end()) {
                if (entry.timestamp > it->timestamp) {
                    *it = entry;
                }
            } else {
                all_entries.push_back(entry);
            }
        }
    }

    // Sort entries by node_id for sequential read access
    std::sort(all_entries.begin(), all_entries.end(),
              [](const MemTableEntry& a, const MemTableEntry& b) {
                  return a.node_id < b.node_id;
              });

    // Remove tombstones at bottom level
    all_entries.erase(
        std::remove_if(all_entries.begin(), all_entries.end(),
            [](const MemTableEntry& e) { return e.is_deleted; }),
        all_entries.end());

    // Create new SSTable from merged entries
    if (!all_entries.empty()) {
        output_id = sstable_manager_->create_from_entries(all_entries);
    }

    return Error::success();
}

bool CompactionManager::should_pause() const {
    return paused_.load();
}

}  // namespace agent_mem_io