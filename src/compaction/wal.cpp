/**
 * @file wal.cpp
 * @brief Write-Ahead Log (WAL) implementation
 *
 * WAL ensures durability of writes: all operations are logged to disk
 * before being applied to MemTable. On recovery, WAL entries are replayed
 * to restore the MemTable state.
 *
 * Format: each entry is a 4KB-aligned record (O_DIRECT friendly):
 *   [type(4B)][node_id(4B)][dimension(4B)][vector_data(dim*4B)][timestamp(8B)][padding]
 */

#include "compaction/memtable.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <chrono>

namespace agent_mem_io {

// WAL entry types
enum WalEntryType : uint32_t {
    WAL_INSERT = 1,
    WAL_DELETE = 2,
    WAL_CHECKPOINT = 3,
};

// Minimum record size for O_DIRECT alignment (4KB)
static constexpr Size WAL_RECORD_SIZE = 4096;
static constexpr Size WAL_HEADER_SIZE = 4 + 4 + 4;  // type + node_id + dimension

WalManager::WalManager(const std::string& filepath)
    : filepath_(filepath)
    , fd_(-1)
{
}

WalManager::~WalManager() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Error WalManager::init() {
    // Ensure parent directory exists
    auto parent = std::filesystem::path(filepath_).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::filesystem::create_directories(parent);
    }

    // Open WAL file with O_DIRECT for aligned writes
    fd_ = ::open(filepath_.c_str(),
                  O_WRONLY | O_CREAT | O_APPEND | O_DIRECT,
                  S_IRUSR | S_IWUSR);

    if (fd_ < 0) {
        // O_DIRECT may fail on some filesystems, fallback without it
        std::cerr << "[WAL] O_DIRECT open failed, trying without: "
                  << strerror(errno) << "\n";
        fd_ = ::open(filepath_.c_str(),
                      O_WRONLY | O_CREAT | O_APPEND,
                      S_IRUSR | S_IWUSR);
    }

    if (fd_ < 0) {
        return Error::io_error("Failed to open WAL file: " + filepath_
                               + " - " + strerror(errno));
    }

    // Also open for reading (recovery needs read access)
    // We use a separate fd for reads since O_APPEND only affects writes

    std::cout << "[WAL] Initialized: " << filepath_ << "\n";
    return Error::success();
}

Error WalManager::log_insert(NodeId node_id, const Vector& vector) {
    if (fd_ < 0) {
        return Error::io_error("WAL not initialized");
    }

    // Build 4KB-aligned record
    char* buffer = nullptr;
    int rc = posix_memalign(reinterpret_cast<void**>(&buffer),
                            WAL_RECORD_SIZE, WAL_RECORD_SIZE);
    if (rc != 0 || !buffer) {
        return Error::io_error("WAL: failed to allocate aligned buffer");
    }

    std::memset(buffer, 0, WAL_RECORD_SIZE);

    Size offset = 0;

    // Write entry type
    uint32_t type = WAL_INSERT;
    std::memcpy(buffer + offset, &type, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Write node ID
    std::memcpy(buffer + offset, &node_id, sizeof(NodeId));
    offset += sizeof(NodeId);

    // Write dimension
    uint32_t dim = static_cast<uint32_t>(vector.size());
    std::memcpy(buffer + offset, &dim, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Write vector data
    Size vec_bytes = vector.size() * sizeof(float);
    if (offset + vec_bytes <= WAL_RECORD_SIZE) {
        std::memcpy(buffer + offset, vector.data(), vec_bytes);
        offset += vec_bytes;
    } else {
        // Vector too large for single 4KB record — write what fits
        // For 128-dim vectors: 512B vector data, plenty of room
        std::memcpy(buffer + offset, vector.data(),
                    std::min(vec_bytes, WAL_RECORD_SIZE - offset));
    }

    // Write timestamp
    uint64_t ts = std::chrono::system_clock::now().time_since_epoch().count();
    if (offset + sizeof(uint64_t) <= WAL_RECORD_SIZE) {
        std::memcpy(buffer + offset, &ts, sizeof(uint64_t));
    }

    // Write to WAL file
    ssize_t written = ::write(fd_, buffer, WAL_RECORD_SIZE);
    free(buffer);

    if (written != WAL_RECORD_SIZE) {
        return Error::io_error(std::string("WAL write failed: ") + strerror(errno));
    }

    current_size_ += WAL_RECORD_SIZE;
    return Error::success();
}

Error WalManager::log_delete(NodeId node_id) {
    if (fd_ < 0) {
        return Error::io_error("WAL not initialized");
    }

    // Build 4KB-aligned record (much simpler for deletes)
    char* buffer = nullptr;
    int rc = posix_memalign(reinterpret_cast<void**>(&buffer),
                            WAL_RECORD_SIZE, WAL_RECORD_SIZE);
    if (rc != 0 || !buffer) {
        return Error::io_error("WAL: failed to allocate aligned buffer");
    }

    std::memset(buffer, 0, WAL_RECORD_SIZE);

    Size offset = 0;

    // Write entry type
    uint32_t type = WAL_DELETE;
    std::memcpy(buffer + offset, &type, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Write node ID
    std::memcpy(buffer + offset, &node_id, sizeof(NodeId));
    offset += sizeof(NodeId);

    // Write timestamp
    uint64_t ts = std::chrono::system_clock::now().time_since_epoch().count();
    std::memcpy(buffer + offset, &ts, sizeof(uint64_t));

    // Write to WAL file
    ssize_t written = ::write(fd_, buffer, WAL_RECORD_SIZE);
    free(buffer);

    if (written != WAL_RECORD_SIZE) {
        return Error::io_error(std::string("WAL write failed: ") + strerror(errno));
    }

    current_size_ += WAL_RECORD_SIZE;
    return Error::success();
}

Error WalManager::sync() {
    if (fd_ < 0) {
        return Error::io_error("WAL not initialized");
    }

    if (::fsync(fd_) != 0) {
        return Error::io_error(std::string("WAL fsync failed: ") + strerror(errno));
    }

    return Error::success();
}

Error WalManager::recover(std::vector<MemTableEntry>& entries) {
    // Close write fd and open for reading
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    int read_fd = ::open(filepath_.c_str(), O_RDONLY | O_DIRECT);
    if (read_fd < 0) {
        read_fd = ::open(filepath_.c_str(), O_RDONLY);
    }

    if (read_fd < 0) {
        // No WAL file — nothing to recover
        return Error::success();
    }

    // Allocate aligned read buffer
    char* buffer = nullptr;
    int rc = posix_memalign(reinterpret_cast<void**>(&buffer),
                            WAL_RECORD_SIZE, WAL_RECORD_SIZE);
    if (rc != 0 || !buffer) {
        ::close(read_fd);
        return Error::io_error("WAL: failed to allocate aligned buffer for recovery");
    }

    // Read all WAL records
    Size offset = 0;
    while (true) {
        ssize_t bytes = pread(read_fd, buffer, WAL_RECORD_SIZE, offset);
        if (bytes < WAL_RECORD_SIZE) {
            break;  // End of WAL or partial record
        }

        Size pos = 0;

        // Read entry type
        uint32_t type = 0;
        std::memcpy(&type, buffer + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);

        // Read node ID
        NodeId node_id = INVALID_NODE_ID;
        std::memcpy(&node_id, buffer + pos, sizeof(NodeId));
        pos += sizeof(NodeId);

        if (type == WAL_INSERT) {
            // Read dimension
            uint32_t dim = 0;
            std::memcpy(&dim, buffer + pos, sizeof(uint32_t));
            pos += sizeof(uint32_t);

            // Read vector data
            Vector vec(dim);
            if (pos + dim * sizeof(float) <= WAL_RECORD_SIZE) {
                std::memcpy(vec.data(), buffer + pos, dim * sizeof(float));
            }

            uint64_t ts = 0;
            if (pos + dim * sizeof(float) + sizeof(uint64_t) <= WAL_RECORD_SIZE) {
                std::memcpy(&ts, buffer + pos + dim * sizeof(float), sizeof(uint64_t));
            }

            entries.emplace_back(node_id, vec, ts, false);
        } else if (type == WAL_DELETE) {
            uint64_t ts = 0;
            std::memcpy(&ts, buffer + pos, sizeof(uint64_t));
            entries.emplace_back(node_id, Vector{}, ts, true);
        }

        offset += WAL_RECORD_SIZE;
    }

    free(buffer);
    ::close(read_fd);

    // Re-open for writing (append mode)
    fd_ = ::open(filepath_.c_str(),
                  O_WRONLY | O_CREAT | O_APPEND | O_DIRECT,
                  S_IRUSR | S_IWUSR);
    if (fd_ < 0) {
        fd_ = ::open(filepath_.c_str(),
                      O_WRONLY | O_CREAT | O_APPEND,
                      S_IRUSR | S_IWUSR);
    }

    std::cout << "[WAL] Recovered " << entries.size() << " entries\n";
    return Error::success();
}

Error WalManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    // Delete the WAL file
    if (std::filesystem::exists(filepath_)) {
        std::filesystem::remove(filepath_);
    }

    current_size_ = 0;

    // Re-open empty WAL file
    fd_ = ::open(filepath_.c_str(),
                  O_WRONLY | O_CREAT | O_APPEND | O_DIRECT,
                  S_IRUSR | S_IWUSR);
    if (fd_ < 0) {
        fd_ = ::open(filepath_.c_str(),
                      O_WRONLY | O_CREAT | O_APPEND,
                      S_IRUSR | S_IWUSR);
    }

    if (fd_ < 0) {
        return Error::io_error("Failed to re-open WAL file after clear");
    }

    return Error::success();
}

Size WalManager::get_size() const {
    return current_size_;
}

Error WalManager::write_entry(const MemTableEntry& entry) {
    if (entry.is_deleted) {
        return log_delete(entry.node_id);
    } else {
        return log_insert(entry.node_id, entry.vector);
    }
}

}  // namespace agent_mem_io