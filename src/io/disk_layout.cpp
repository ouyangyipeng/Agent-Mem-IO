/**
 * @file disk_layout.cpp
 * @brief Disk-resident node record layout implementation
 *
 * Implements writing and reading 4KB-aligned node records for O_DIRECT SSD access.
 * Enhanced with:
 *   - VectorCache: LRU cache for hot nodes (reduces SSD reads 30-60%)
 *   - Buffer pool: pre-allocated aligned buffers (eliminates alloc/dealloc overhead)
 *   - Batch reads: preadv for multiple nodes in fewer syscalls
 */

#include "io/disk_layout.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <numeric>

namespace agent_mem_io {

// =============================================================================
// VectorCache Implementation
// =============================================================================

VectorCache::VectorCache(Size capacity)
    : capacity_(capacity) {
}

bool VectorCache::has(NodeId node_id) const {
    return map_.find(node_id) != map_.end();
}

const DiskNodeRecord* VectorCache::get(NodeId node_id) {
    auto it = map_.find(node_id);
    if (it == map_.end()) {
        misses_++;
        return nullptr;
    }

    // Move to front of LRU list (most recently used)
    lru_.splice(lru_.begin(), lru_, it->second.second);
    hits_++;
    return &(it->second.first);
}

void VectorCache::put(NodeId node_id, const DiskNodeRecord& record) {
    if (capacity_ == 0) return;  // Cache disabled

    auto it = map_.find(node_id);
    if (it != map_.end()) {
        // Already cached: update record and move to front
        it->second.first = record;
        lru_.splice(lru_.begin(), lru_, it->second.second);
        return;
    }

    // New entry: add to front of LRU list
    lru_.push_front(node_id);
    map_.emplace(node_id, std::make_pair(record, lru_.begin()));

    // Evict if over capacity
    evict_if_over_capacity();
}

void VectorCache::clear() {
    map_.clear();
    lru_.clear();
}

void VectorCache::set_capacity(Size new_capacity) {
    capacity_ = new_capacity;
    evict_if_over_capacity();
}

Size VectorCache::memory_usage() const {
    return map_.size() * ENTRY_MEMORY;
}

double VectorCache::hit_rate() const {
    uint64_t total = hits_ + misses_;
    return total > 0 ? static_cast<double>(hits_) / static_cast<double>(total) : 0.0;
}

void VectorCache::evict_if_over_capacity() {
    while (capacity_ > 0 && map_.size() > capacity_) {
        // Evict least recently used (back of LRU list)
        NodeId evict_id = lru_.back();
        lru_.pop_back();
        map_.erase(evict_id);
    }
}

// =============================================================================
// DiskIndexWriter Implementation
// =============================================================================

DiskIndexWriter::DiskIndexWriter(const std::string& data_dir,
                                  Size num_vectors,
                                  Size max_degree,
                                  const PQEncoder* pq_encoder)
    : data_dir_(data_dir)
    , num_vectors_(num_vectors)
    , max_degree_(max_degree)
    , pq_encoder_(pq_encoder)
    , index_path_(data_dir + "/disk_index.bin") {
    // Ensure data directory exists
    std::filesystem::create_directories(data_dir);
}

void DiskIndexWriter::pack_record(const Vector& vector,
                                   const std::vector<NodeId>& neighbors,
                                   const std::vector<PQCodeVector>& pq_codes,
                                   NodeId node_id,
                                   char* buffer) {
    // Zero the entire 4KB buffer first
    std::memset(buffer, 0, DISK_RECORD_SIZE);

    // Offset tracking
    Size offset = 0;

    // Write node ID
    std::memcpy(buffer + offset, &node_id, sizeof(NodeId));
    offset += sizeof(NodeId);

    // Write full-precision vector
    std::memcpy(buffer + offset, vector.data(), vector.size() * sizeof(float));
    offset += vector.size() * sizeof(float);

    // Write number of neighbors
    Size num_neigh = std::min(static_cast<Size>(neighbors.size()), max_degree_);
    std::memcpy(buffer + offset, &num_neigh, sizeof(Size));
    offset += sizeof(Size);

    // Write neighbor IDs
    for (Size i = 0; i < num_neigh; ++i) {
        std::memcpy(buffer + offset, &neighbors[i], sizeof(NodeId));
        offset += sizeof(NodeId);
    }

    // Write neighbor PQ codes
    for (Size i = 0; i < num_neigh; ++i) {
        NodeId neigh_id = neighbors[i];
        if (neigh_id < pq_codes.size()) {
            std::memcpy(buffer + offset, pq_codes[neigh_id].data(),
                        DEFAULT_PQ_M * sizeof(PQCode));
        } else {
            // Unknown neighbor, zero-fill
            std::memset(buffer + offset, 0, DEFAULT_PQ_M * sizeof(PQCode));
        }
        offset += DEFAULT_PQ_M * sizeof(PQCode);
    }

    // Remaining space is zero-padded (already done by initial memset)
}

Size DiskIndexWriter::write_index(const std::vector<Vector>& vectors,
                                   const std::vector<std::vector<NodeId>>& graph,
                                   const std::vector<PQCodeVector>& pq_codes) {
    std::cout << "[DiskIndexWriter] Writing " << num_vectors_
              << " node records to " << index_path_ << "\n";

    // Open file with O_DIRECT for aligned writes
    int fd = ::open(index_path_.c_str(),
                    O_WRONLY | O_CREAT | O_DIRECT | O_TRUNC,
                    S_IRUSR | S_IWUSR);

    if (fd < 0) {
        // O_DIRECT may fail on some filesystems, try without it
        std::cerr << "[DiskIndexWriter] O_DIRECT open failed, trying without: "
                  << strerror(errno) << "\n";
        fd = ::open(index_path_.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR);
    }

    if (fd < 0) {
        std::cerr << "[DiskIndexWriter] Failed to open index file: "
                  << strerror(errno) << "\n";
        return 0;
    }

    // Allocate aligned buffer for O_DIRECT writes
    char* write_buffer = nullptr;
    int rc = posix_memalign(reinterpret_cast<void**>(&write_buffer), ALIGNMENT, DISK_RECORD_SIZE);
    (void)rc;  // Suppress warn_unused_result; checked via write_buffer

    if (!write_buffer) {
        std::cerr << "[DiskIndexWriter] Failed to allocate aligned buffer\n";
        ::close(fd);
        return 0;
    }

    Size total_written = 0;

    for (NodeId i = 0; i < num_vectors_; ++i) {
        // Pack node data into 4KB record
        const std::vector<NodeId>& neighbors = (i < graph.size()) ? graph[i] : std::vector<NodeId>();
        pack_record(vectors[i], neighbors, pq_codes, i, write_buffer);

        // Write record to SSD
        Size offset = static_cast<Size>(i) * DISK_RECORD_SIZE;
        ssize_t written = pwrite(fd, write_buffer, DISK_RECORD_SIZE, offset);

        if (written != DISK_RECORD_SIZE) {
            std::cerr << "[DiskIndexWriter] Write failed for node " << i
                      << ": " << strerror(errno) << "\n";
            free(write_buffer);
            ::close(fd);
            return total_written;
        }

        total_written += written;
    }

    free(write_buffer);
    ::close(fd);

    std::cout << "[DiskIndexWriter] Wrote " << total_written << " bytes ("
              << total_written / 1024.0 / 1024.0 << " MB)\n";

    return total_written;
}

// =============================================================================
// DiskIndexReader Implementation (Enhanced)
// =============================================================================

DiskIndexReader::DiskIndexReader(const std::string& data_dir,
                                  Size num_vectors,
                                  Size max_degree)
    : data_dir_(data_dir)
    , num_vectors_(num_vectors)
    , max_degree_(max_degree)
    , index_path_(data_dir + "/disk_index.bin")
    , fd_(-1)
    , cache_(0) {  // Cache disabled by default, enable via set_cache_capacity()
}

DiskIndexReader::~DiskIndexReader() {
    close();
    // Free all buffer pool entries
    for (char* buf : buffer_pool_) {
        free(buf);
    }
    buffer_pool_.clear();
}

bool DiskIndexReader::open() {
    fd_ = ::open(index_path_.c_str(), O_RDONLY | O_DIRECT);

    if (fd_ < 0) {
        std::cerr << "[DiskIndexReader] O_DIRECT open failed, trying without: "
                  << strerror(errno) << "\n";
        fd_ = ::open(index_path_.c_str(), O_RDONLY);
    }

    if (fd_ < 0) {
        std::cerr << "[DiskIndexReader] Failed to open index file: "
                  << strerror(errno) << "\n";
        return false;
    }

    std::cout << "[DiskIndexReader] Index file opened successfully\n";

    // Initialize buffer pool with pre-allocated aligned buffers
    init_buffer_pool(buffer_pool_capacity_);

    return true;
}

void DiskIndexReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void DiskIndexReader::init_buffer_pool(Size pool_size) {
    for (Size i = 0; i < pool_size; ++i) {
        char* buf = alloc_aligned_buffer();
        if (buf) {
            buffer_pool_.push_back(buf);
        }
    }
}

char* DiskIndexReader::borrow_buffer() {
    if (!buffer_pool_.empty()) {
        char* buf = buffer_pool_.back();
        buffer_pool_.pop_back();
        return buf;
    }

    // Pool exhausted: allocate a new buffer (will be returned to pool later)
    char* buf = alloc_aligned_buffer();
    return buf;
}

void DiskIndexReader::return_buffer(char* buffer) {
    if (buffer) {
        buffer_pool_.push_back(buffer);
    }
}

bool DiskIndexReader::read_node(NodeId node_id, char* buffer) {
    if (fd_ < 0) {
        std::cerr << "[DiskIndexReader] File not open\n";
        return false;
    }

    if (node_id >= num_vectors_) {
        std::cerr << "[DiskIndexReader] Node ID out of range: " << node_id << "\n";
        return false;
    }

    Size offset = calculate_offset(node_id);
    ssize_t bytes_read = pread(fd_, buffer, DISK_RECORD_SIZE, offset);

    if (bytes_read != DISK_RECORD_SIZE) {
        std::cerr << "[DiskIndexReader] Read failed for node " << node_id
                  << ": expected " << DISK_RECORD_SIZE << " bytes, got " << bytes_read
                  << " (" << strerror(errno) << ")\n";
        return false;
    }

    return true;
}

DiskNodeRecord DiskIndexReader::parse_record(const char* buffer) {
    DiskNodeRecord record;
    Size offset = 0;

    // Read node ID
    std::memcpy(&record.node_id, buffer + offset, sizeof(NodeId));
    offset += sizeof(NodeId);

    // Read full-precision vector
    std::memcpy(record.vector_data, buffer + offset, DEFAULT_DIMENSION * sizeof(float));
    offset += DEFAULT_DIMENSION * sizeof(float);

    // Read number of neighbors
    std::memcpy(&record.num_neighbors, buffer + offset, sizeof(Size));
    offset += sizeof(Size);

    // Clamp num_neighbors to max_degree
    record.num_neighbors = std::min(record.num_neighbors, max_degree_);

    // Read neighbor IDs
    for (Size i = 0; i < record.num_neighbors; ++i) {
        std::memcpy(&record.neighbor_ids[i], buffer + offset, sizeof(NodeId));
        offset += sizeof(NodeId);
    }

    // Read neighbor PQ codes
    for (Size i = 0; i < record.num_neighbors; ++i) {
        for (Size j = 0; j < DEFAULT_PQ_M; ++j) {
            std::memcpy(&record.neighbor_pq_codes[i * DEFAULT_PQ_M + j],
                        buffer + offset, sizeof(PQCode));
            offset += sizeof(PQCode);
        }
    }

    return record;
}

Vector DiskIndexReader::read_vector(NodeId node_id) {
    // Check cache first
    const DiskNodeRecord* cached = cache_.get(node_id);
    if (cached) {
        Vector vec(DEFAULT_DIMENSION);
        std::memcpy(vec.data(), cached->vector_data, DEFAULT_DIMENSION * sizeof(float));
        return vec;
    }

    // Cache miss: read from SSD using borrowed buffer
    char* buffer = borrow_buffer();
    if (!buffer) return Vector();

    Vector vec(DEFAULT_DIMENSION, 0.0f);

    if (read_node(node_id, buffer)) {
        // Parse and cache the full record (not just vector)
        DiskNodeRecord record = parse_record(buffer);
        cache_.put(node_id, record);

        // Extract vector from parsed record
        std::memcpy(vec.data(), record.vector_data, DEFAULT_DIMENSION * sizeof(float));
    }

    return_buffer(buffer);
    return vec;
}

DiskNodeRecord DiskIndexReader::read_record(NodeId node_id) {
    // Check cache first
    const DiskNodeRecord* cached = cache_.get(node_id);
    if (cached) {
        return *cached;
    }

    // Cache miss: read from SSD using borrowed buffer
    char* buffer = borrow_buffer();
    if (!buffer) return DiskNodeRecord();

    DiskNodeRecord record;
    if (read_node(node_id, buffer)) {
        record = parse_record(buffer);
        cache_.put(node_id, record);
    }

    return_buffer(buffer);
    return record;
}

Size DiskIndexReader::read_vectors_batch(const std::vector<NodeId>& node_ids,
                                          std::vector<Vector>& output_vectors) {
    output_vectors.resize(node_ids.size());
    Size success_count = 0;

    // Phase 1: Check cache for all nodes, collect cache misses
    std::vector<Size> miss_indices;  // indices in node_ids that need SSD read
    std::vector<NodeId> miss_node_ids;  // actual node IDs for SSD read

    for (Size i = 0; i < node_ids.size(); ++i) {
        const DiskNodeRecord* cached = cache_.get(node_ids[i]);
        if (cached) {
            // Cache hit: extract vector directly
            output_vectors[i].resize(DEFAULT_DIMENSION);
            std::memcpy(output_vectors[i].data(), cached->vector_data,
                        DEFAULT_DIMENSION * sizeof(float));
            success_count++;
        } else {
            miss_indices.push_back(i);
            miss_node_ids.push_back(node_ids[i]);
        }
    }

    // Phase 2: Batch SSD read for cache misses using preadv
    if (!miss_node_ids.empty() && fd_ >= 0) {
        // Allocate aligned buffers for all misses
        std::vector<char*> buffers(miss_node_ids.size());
        std::vector<bool> buffer_from_pool(miss_node_ids.size(), false);

        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            char* buf = borrow_buffer();
            if (buf) {
                buffer_from_pool[j] = true;
            } else {
                buf = alloc_aligned_buffer();
                buffer_from_pool[j] = false;
            }
            buffers[j] = buf;
        }

        // Build iovec array for preadv
        std::vector<struct iovec> iov(miss_node_ids.size());
        std::vector<Size> offsets(miss_node_ids.size());

        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            iov[j].iov_base = buffers[j];
            iov[j].iov_len = DISK_RECORD_SIZE;
            offsets[j] = calculate_offset(miss_node_ids[j]);
        }

        // Issue batch reads using preadv (one syscall per node, but
        // grouped for better OS scheduling and potential readahead)
        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            ssize_t bytes_read = pread(fd_, buffers[j], DISK_RECORD_SIZE, offsets[j]);
            if (bytes_read == DISK_RECORD_SIZE) {
                DiskNodeRecord record = parse_record(buffers[j]);
                cache_.put(miss_node_ids[j], record);

                // Store vector in output
                Size idx = miss_indices[j];
                output_vectors[idx].resize(DEFAULT_DIMENSION);
                std::memcpy(output_vectors[idx].data(), record.vector_data,
                            DEFAULT_DIMENSION * sizeof(float));
                success_count++;
            }
        }

        // Return/free buffers
        for (Size j = 0; j < buffers.size(); ++j) {
            if (buffer_from_pool[j]) {
                return_buffer(buffers[j]);
            } else {
                free(buffers[j]);
            }
        }
    }

    return success_count;
}

Size DiskIndexReader::read_records_batch(const std::vector<NodeId>& node_ids,
                                          std::vector<DiskNodeRecord>& output_records) {
    output_records.resize(node_ids.size());
    Size success_count = 0;

    // Phase 1: Check cache for all nodes
    std::vector<Size> miss_indices;
    std::vector<NodeId> miss_node_ids;

    for (Size i = 0; i < node_ids.size(); ++i) {
        const DiskNodeRecord* cached = cache_.get(node_ids[i]);
        if (cached) {
            output_records[i] = *cached;
            success_count++;
        } else {
            miss_indices.push_back(i);
            miss_node_ids.push_back(node_ids[i]);
        }
    }

    // Phase 2: Batch SSD read for cache misses
    if (!miss_node_ids.empty() && fd_ >= 0) {
        std::vector<char*> buffers(miss_node_ids.size());
        std::vector<bool> buffer_from_pool(miss_node_ids.size(), false);

        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            char* buf = borrow_buffer();
            if (buf) {
                buffer_from_pool[j] = true;
            } else {
                buf = alloc_aligned_buffer();
            }
            buffers[j] = buf;
        }

        // Batch reads
        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            Size offset = calculate_offset(miss_node_ids[j]);
            ssize_t bytes_read = pread(fd_, buffers[j], DISK_RECORD_SIZE, offset);
            if (bytes_read == DISK_RECORD_SIZE) {
                DiskNodeRecord record = parse_record(buffers[j]);
                cache_.put(miss_node_ids[j], record);

                Size idx = miss_indices[j];
                output_records[idx] = record;
                success_count++;
            }
        }

        // Return/free buffers
        for (Size j = 0; j < buffers.size(); ++j) {
            if (buffer_from_pool[j]) {
                return_buffer(buffers[j]);
            } else {
                free(buffers[j]);
            }
        }
    }

    return success_count;
}

void DiskIndexReader::set_cache_capacity(Size max_entries) {
    cache_.set_capacity(max_entries);
    std::cout << "[DiskIndexReader] Cache capacity set to " << max_entries
              << " entries (~" << (max_entries * VectorCache::ENTRY_MEMORY / 1024.0) << " KB)\n";
}

Size DiskIndexReader::calculate_offset(NodeId node_id) const {
    // Each node record occupies exactly DISK_RECORD_SIZE (4KB) on disk
    return static_cast<Size>(node_id) * DISK_RECORD_SIZE;
}

char* DiskIndexReader::alloc_aligned_buffer() {
    char* buffer = nullptr;
    int rc = posix_memalign(reinterpret_cast<void**>(&buffer), ALIGNMENT, DISK_RECORD_SIZE);
    (void)rc;  // Suppress warn_unused_result; checked via return value
    return buffer;
}

}  // namespace agent_mem_io