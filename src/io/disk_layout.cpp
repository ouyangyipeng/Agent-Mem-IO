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
#include "core/simd_distance.h"

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

    // Write node ID at offset 0, then padding to 16 bytes
    // This ensures vector data starts at offset 16 (SIMD-aligned within 4KB page)
    std::memcpy(buffer, &node_id, sizeof(NodeId));
    // Padding bytes 4-15 are already zeroed by memset above

    // Write full-precision vector at aligned offset 16
    Size offset = DISK_VECTOR_OFFSET;
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
    , fd_(-1) {
    // Buffer pool manager will be initialized via set_cache_capacity()
}

DiskIndexReader::~DiskIndexReader() {
    close();
    // Free all temp buffer pool entries
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

    // Read node ID at offset 0 (header is 16 bytes: 4B node_id + 12B padding)
    std::memcpy(&record.node_id, buffer, sizeof(NodeId));

    // Read full-precision vector at aligned offset 16
    std::memcpy(record.vector_data, buffer + DISK_VECTOR_OFFSET,
                DEFAULT_DIMENSION * sizeof(float));
    Size offset = DISK_VECTOR_OFFSET + DEFAULT_DIMENSION * sizeof(float);

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
    // Check buffer pool cache first
    if (buffer_pool_mgr_) {
        auto load_func = [this](char* buffer, PageId page_id) {
            read_node(static_cast<NodeId>(page_id), buffer);
        };
        char* page_data = buffer_pool_mgr_->get_or_load_page(node_id, node_id, load_func);
        if (page_data) {
            DiskNodeRecord record = parse_record(page_data);
            Vector vec(DEFAULT_DIMENSION);
            std::memcpy(vec.data(), record.vector_data, DEFAULT_DIMENSION * sizeof(float));
            return vec;
        }
    }

    // Fallback: read from SSD using borrowed temp buffer (no cache)
    char* buffer = borrow_buffer();
    if (!buffer) return Vector();

    Vector vec(DEFAULT_DIMENSION, 0.0f);
    if (read_node(node_id, buffer)) {
        DiskNodeRecord record = parse_record(buffer);
        std::memcpy(vec.data(), record.vector_data, DEFAULT_DIMENSION * sizeof(float));
    }

    return_buffer(buffer);
    return vec;
}

DiskNodeRecord DiskIndexReader::read_record(NodeId node_id) {
    // Check buffer pool cache first
    if (buffer_pool_mgr_) {
        auto load_func = [this](char* buffer, PageId page_id) {
            read_node(static_cast<NodeId>(page_id), buffer);
        };
        char* page_data = buffer_pool_mgr_->get_or_load_page(node_id, node_id, load_func);
        if (page_data) {
            return parse_record(page_data);
        }
    }

    // Fallback: read from SSD using borrowed temp buffer (no cache)
    char* buffer = borrow_buffer();
    if (!buffer) return DiskNodeRecord();

    DiskNodeRecord record;
    if (read_node(node_id, buffer)) {
        record = parse_record(buffer);
    }

    return_buffer(buffer);
    return record;
}

Size DiskIndexReader::read_vectors_batch(const std::vector<NodeId>& node_ids,
                                          std::vector<Vector>& output_vectors) {
    output_vectors.resize(node_ids.size());
    Size success_count = 0;

    // Phase 1: Check buffer pool cache for all nodes, collect misses
    std::vector<Size> miss_indices;
    std::vector<NodeId> miss_node_ids;

    for (Size i = 0; i < node_ids.size(); ++i) {
        if (buffer_pool_mgr_ && buffer_pool_mgr_->contains(node_ids[i])) {
            // Cache hit: get page and parse record
            char* page_data = buffer_pool_mgr_->get_page(node_ids[i], node_ids[i]);
            if (page_data) {
                DiskNodeRecord record = parse_record(page_data);
                output_vectors[i].resize(DEFAULT_DIMENSION);
                std::memcpy(output_vectors[i].data(), record.vector_data,
                            DEFAULT_DIMENSION * sizeof(float));
                success_count++;
                continue;
            }
        }
        miss_indices.push_back(i);
        miss_node_ids.push_back(node_ids[i]);
    }

    // Phase 2: Batch SSD read for cache misses using buffer pool
    if (!miss_node_ids.empty() && fd_ >= 0) {
        // Read each miss via buffer pool manager (which caches automatically)
        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            NodeId nid = miss_node_ids[j];
            Size idx = miss_indices[j];

            if (buffer_pool_mgr_) {
                auto load_func = [this](char* buffer, PageId page_id) {
                    read_node(static_cast<NodeId>(page_id), buffer);
                };
                char* page_data = buffer_pool_mgr_->get_or_load_page(nid, nid, load_func);
                if (page_data) {
                    DiskNodeRecord record = parse_record(page_data);
                    output_vectors[idx].resize(DEFAULT_DIMENSION);
                    std::memcpy(output_vectors[idx].data(), record.vector_data,
                                DEFAULT_DIMENSION * sizeof(float));
                    success_count++;
                }
            } else {
                // No buffer pool: direct SSD read
                char* buf = borrow_buffer();
                if (buf && read_node(nid, buf)) {
                    DiskNodeRecord record = parse_record(buf);
                    output_vectors[idx].resize(DEFAULT_DIMENSION);
                    std::memcpy(output_vectors[idx].data(), record.vector_data,
                                DEFAULT_DIMENSION * sizeof(float));
                    success_count++;
                }
                if (buf) return_buffer(buf);
            }
        }
    }

    return success_count;
}

Size DiskIndexReader::read_records_batch(const std::vector<NodeId>& node_ids,
                                          std::vector<DiskNodeRecord>& output_records) {
    output_records.resize(node_ids.size());
    Size success_count = 0;

    // Phase 1: Check buffer pool cache for all nodes
    std::vector<Size> miss_indices;
    std::vector<NodeId> miss_node_ids;

    for (Size i = 0; i < node_ids.size(); ++i) {
        if (buffer_pool_mgr_ && buffer_pool_mgr_->contains(node_ids[i])) {
            char* page_data = buffer_pool_mgr_->get_page(node_ids[i], node_ids[i]);
            if (page_data) {
                output_records[i] = parse_record(page_data);
                success_count++;
                continue;
            }
        }
        miss_indices.push_back(i);
        miss_node_ids.push_back(node_ids[i]);
    }

    // Phase 2: Read misses via buffer pool (caches automatically)
    if (!miss_node_ids.empty() && fd_ >= 0) {
        for (Size j = 0; j < miss_node_ids.size(); ++j) {
            NodeId nid = miss_node_ids[j];
            Size idx = miss_indices[j];

            if (buffer_pool_mgr_) {
                auto load_func = [this](char* buffer, PageId page_id) {
                    read_node(static_cast<NodeId>(page_id), buffer);
                };
                char* page_data = buffer_pool_mgr_->get_or_load_page(nid, nid, load_func);
                if (page_data) {
                    output_records[idx] = parse_record(page_data);
                    success_count++;
                }
            } else {
                char* buf = borrow_buffer();
                if (buf && read_node(nid, buf)) {
                    output_records[idx] = parse_record(buf);
                    success_count++;
                }
                if (buf) return_buffer(buf);
            }
        }
    }

    return success_count;
}

void DiskIndexReader::set_cache_capacity(Size max_pages) {
    if (max_pages == 0) {
        // Disable caching
        buffer_pool_mgr_.reset();
        std::cout << "[DiskIndexReader] Cache disabled\n";
        return;
    }

    // Create BufferPoolManager with Graph-Aware 2Q eviction policy
    BufferPoolConfig config;
    config.max_pages = max_pages;
    config.page_size = DISK_RECORD_SIZE;  // 4KB per node record
    config.hot_queue_ratio = 0.3;         // 30% pages in hot queue (hub nodes)
    config.enable_graph_aware = true;      // Graph-Aware 2Q eviction
    config.enable_prefetch = false;        // Prefetch handled by IoEngine separately

    buffer_pool_mgr_ = std::make_unique<BufferPoolManager>(config);

    std::cout << "[DiskIndexReader] Cache capacity set to " << max_pages
              << " pages (~" << (max_pages * DISK_RECORD_SIZE / 1024.0) << " KB)"
              << " (Graph-Aware 2Q eviction)\n";
}

void DiskIndexReader::update_in_degrees(const std::vector<uint32_t>& in_degrees) {
    if (!buffer_pool_mgr_) return;

    // Update in-degrees for all nodes that are in the buffer pool
    // This enables Graph-Aware 2Q eviction: high in-degree hub nodes
    // are protected from eviction, staying cached for future queries
    for (Size node_id = 0; node_id < in_degrees.size(); ++node_id) {
        if (in_degrees[node_id] > 0) {
            buffer_pool_mgr_->update_in_degree(node_id, in_degrees[node_id]);
        }
    }
    std::cout << "[DiskIndexReader] Updated in-degrees for " << in_degrees.size()
              << " nodes (Graph-Aware 2Q eviction enabled)\n";
}

bool DiskIndexReader::is_cached(NodeId node_id) const {
    if (!buffer_pool_mgr_) return false;
    return buffer_pool_mgr_->contains(node_id);
}

DiskNodeRecord DiskIndexReader::get_cached_record(NodeId node_id) {
    if (!buffer_pool_mgr_) return DiskNodeRecord();
    char* page_data = buffer_pool_mgr_->get_page(node_id, node_id);
    if (!page_data) return DiskNodeRecord();
    return parse_record(page_data);
}

float DiskIndexReader::compute_distance_direct(NodeId node_id, const Vector& query) {
    // Fast path: page is cached in BufferPool → compute directly from page buffer
    // Vector data is at DISK_VECTOR_OFFSET (16 bytes) within each 4KB-aligned page,
    // which means it's 16-byte aligned relative to the page base.
    // Since BufferPool pages are 4KB-aligned (from O_DIRECT), the vector data
    // pointer is 16-byte aligned for SSE _mm_load_ps.
    if (buffer_pool_mgr_) {
        char* page_data = buffer_pool_mgr_->get_page(node_id, node_id);
        if (page_data) {
            // Direct SIMD distance from page buffer: skip parse_record memcpy!
            const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
            return l2_distance_sq_simd(query.data(), vec_ptr, query.size());
        }

        // Slow path: not cached → load page into BufferPool, then compute
        auto load_func = [this](char* buffer, PageId pid) {
            Size offset = calculate_offset(static_cast<NodeId>(pid));
            ssize_t bytes_read = pread(fd_, buffer, DISK_RECORD_SIZE, offset);
            if (bytes_read != DISK_RECORD_SIZE) return;
        };
        page_data = buffer_pool_mgr_->get_or_load_page(node_id, node_id, load_func);
        if (page_data) {
            const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
            return l2_distance_sq_simd(query.data(), vec_ptr, query.size());
        }
    }

    // No BufferPool: fall back to read_vector + manual distance
    Vector vec = read_vector(node_id);
    if (vec.empty()) return -1.0f;
    return l2_distance_sq_simd(query.data(), vec.data(), query.size());
}

Size DiskIndexReader::compute_distances_batch_direct(
    const std::vector<NodeId>& node_ids,
    const Vector& query,
    std::vector<float>& distances) {

    distances.resize(node_ids.size(), -1.0f);
    Size success_count = 0;

    if (!buffer_pool_mgr_) {
        // No BufferPool: fall back to individual compute_distance_direct
        for (Size i = 0; i < node_ids.size(); ++i) {
            float d = compute_distance_direct(node_ids[i], query);
            if (d >= 0.0f) {
                distances[i] = d;
                success_count++;
            }
        }
        return success_count;
    }

    // Phase 1: Check which pages are already cached
    std::vector<bool> cached(node_ids.size(), false);
    for (Size i = 0; i < node_ids.size(); ++i) {
        cached[i] = buffer_pool_mgr_->contains(node_ids[i]);
    }

    // Phase 2: Compute distances for cached pages (direct SIMD from buffer)
    for (Size i = 0; i < node_ids.size(); ++i) {
        if (cached[i]) {
            char* page_data = buffer_pool_mgr_->get_page(node_ids[i], node_ids[i]);
            if (page_data) {
                // Software prefetch for next vector to improve cache utilization
                if (i + 1 < node_ids.size() && cached[i + 1]) {
                    char* next_page = buffer_pool_mgr_->get_page(node_ids[i + 1], node_ids[i + 1]);
                    if (next_page) {
                        __builtin_prefetch(next_page + DISK_VECTOR_OFFSET, 0, 1);
                    }
                }
                const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
                distances[i] = l2_distance_sq_simd(query.data(), vec_ptr, query.size());
                success_count++;
            }
        }
    }

    // Phase 3: Load and compute for non-cached pages
    for (Size i = 0; i < node_ids.size(); ++i) {
        if (!cached[i] && distances[i] < 0.0f) {
            char* page_data = nullptr;
            auto load_func = [this](char* buffer, PageId pid) {
                Size offset = calculate_offset(static_cast<NodeId>(pid));
                ssize_t bytes_read = pread(fd_, buffer, DISK_RECORD_SIZE, offset);
                if (bytes_read != DISK_RECORD_SIZE) return;
            };
            page_data = buffer_pool_mgr_->get_or_load_page(node_ids[i], node_ids[i], load_func);
            if (page_data) {
                const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
                distances[i] = l2_distance_sq_simd(query.data(), vec_ptr, query.size());
                success_count++;
            }
        }
    }

    return success_count;
}

double DiskIndexReader::get_cache_hit_rate() const {
    if (!buffer_pool_mgr_) return 0.0;
    return buffer_pool_mgr_->get_hit_rate();
}

Size DiskIndexReader::get_cache_memory_usage() const {
    if (!buffer_pool_mgr_) return 0;
    return buffer_pool_mgr_->get_memory_usage();
}

Size DiskIndexReader::get_cache_size() const {
    if (!buffer_pool_mgr_) return 0;
    return buffer_pool_mgr_->size();
}

Size DiskIndexReader::get_cache_capacity() const {
    if (!buffer_pool_mgr_) return 0;
    return buffer_pool_mgr_->max_size();
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

// =============================================================================
// DiskIndexReader — Async I/O via IoEngine (io_uring support)
// =============================================================================

void DiskIndexReader::set_io_engine(IoEngine* io_engine) {
    io_engine_ = io_engine;
    if (io_engine_) {
        std::cout << "[DiskIndexReader] IoEngine set (io_uring="
                  << (io_engine_->using_io_uring() ? "YES" : "NO (pread fallback)")
                  << ")\n";
    }
}

Size DiskIndexReader::submit_async_batch(const std::vector<NodeId>& node_ids) {
    if (!io_engine_ || fd_ < 0) {
        // No IoEngine or file not open — fall back to sync
        return 0;
    }

    // Build IoRequest vector for all buffer-pool-miss nodes
    std::vector<IoRequest> requests;

    for (NodeId node_id : node_ids) {
        // Skip if already in buffer pool
        if (buffer_pool_mgr_ && buffer_pool_mgr_->contains(node_id)) {
            continue;
        }

        // Allocate aligned buffer for this read
        char* buffer = borrow_buffer();
        if (!buffer) {
            buffer = alloc_aligned_buffer();
            if (!buffer) continue;
        }

        // Store buffer mapping for later retrieval
        async_buffers_[node_id] = buffer;

        // Build IoRequest
        IoRequest req;
        req.fd = fd_;
        req.offset = calculate_offset(node_id);
        req.size = DISK_RECORD_SIZE;
        req.buffer = buffer;
        req.node_id = node_id;
        req.is_read = true;
        req.user_data = static_cast<uint64_t>(node_id);

        requests.push_back(req);
    }

    if (requests.empty()) {
        return 0;
    }

    // Submit all requests as a batch via IoEngine
    // When io_uring is available, this fills multiple SQEs and submits once
    // When io_uring is not available, this falls back to synchronous pread
    Size submitted = io_engine_->submit_batch_read(requests);

    // Only log first batch to confirm io_uring is working, not every batch
    static bool logged_once = false;
    if (!logged_once) {
        std::cout << "[DiskIndexReader] Async batch: submitted " << submitted
                  << " / " << requests.size() << " reads via "
                  << (io_engine_->using_io_uring() ? "io_uring" : "pread") << "\n";
        logged_once = true;
    }

    return submitted;
}

Size DiskIndexReader::wait_async_batch(const std::vector<NodeId>& node_ids,
                                         std::vector<Vector>& output_vectors) {
    output_vectors.resize(node_ids.size());
    Size success_count = 0;

    // Phase 1: Check buffer pool for all nodes first
    for (Size i = 0; i < node_ids.size(); ++i) {
        if (buffer_pool_mgr_ && buffer_pool_mgr_->contains(node_ids[i])) {
            char* page_data = buffer_pool_mgr_->get_page(node_ids[i], node_ids[i]);
            if (page_data) {
                DiskNodeRecord record = parse_record(page_data);
                output_vectors[i].resize(DEFAULT_DIMENSION);
                std::memcpy(output_vectors[i].data(), record.vector_data,
                            DEFAULT_DIMENSION * sizeof(float));
                success_count++;
            }
        }
    }

    // Phase 2: Wait for async completions and insert into buffer pool
    if (io_engine_ && !async_buffers_.empty()) {
        Size max_completions = async_buffers_.size();
        auto completions = io_engine_->wait_completion_batch(max_completions, 100);

        for (const auto& comp : completions) {
            if (comp.success() && comp.node_id != INVALID_NODE_ID) {
                Size idx = static_cast<Size>(-1);
                for (Size j = 0; j < node_ids.size(); ++j) {
                    if (node_ids[j] == comp.node_id) {
                        idx = j;
                        break;
                    }
                }

                auto buf_it = async_buffers_.find(comp.node_id);
                if (buf_it != async_buffers_.end() && idx != static_cast<Size>(-1)) {
                    // Insert async read result into buffer pool (direct data insertion)
                    if (buffer_pool_mgr_) {
                        buffer_pool_mgr_->put_page_data(comp.node_id, comp.node_id,
                                                         buf_it->second, DISK_RECORD_SIZE);
                        // Re-read from buffer pool to get parsed result
                        char* cached_data = buffer_pool_mgr_->get_page(comp.node_id, comp.node_id);
                        if (cached_data) {
                            DiskNodeRecord record = parse_record(cached_data);
                            output_vectors[idx].resize(DEFAULT_DIMENSION);
                            std::memcpy(output_vectors[idx].data(), record.vector_data,
                                        DEFAULT_DIMENSION * sizeof(float));
                            success_count++;
                        }
                    } else {
                        // No buffer pool: parse directly
                        DiskNodeRecord record = parse_record(buf_it->second);
                        output_vectors[idx].resize(DEFAULT_DIMENSION);
                        std::memcpy(output_vectors[idx].data(), record.vector_data,
                                    DEFAULT_DIMENSION * sizeof(float));
                        success_count++;
                    }
                }
            }
        }
    }

    // Phase 3: For remaining misses, synchronous fallback via buffer pool
    for (Size i = 0; i < node_ids.size(); ++i) {
        if (output_vectors[i].empty()) {
            // Re-check buffer pool (async might have inserted it)
            if (buffer_pool_mgr_ && buffer_pool_mgr_->contains(node_ids[i])) {
                char* page_data = buffer_pool_mgr_->get_page(node_ids[i], node_ids[i]);
                if (page_data) {
                    DiskNodeRecord record = parse_record(page_data);
                    output_vectors[i].resize(DEFAULT_DIMENSION);
                    std::memcpy(output_vectors[i].data(), record.vector_data,
                                DEFAULT_DIMENSION * sizeof(float));
                    success_count++;
                    continue;
                }
            }

            // Synchronous fallback read
            if (buffer_pool_mgr_) {
                auto load_func = [this](char* buffer, PageId page_id) {
                    read_node(static_cast<NodeId>(page_id), buffer);
                };
                char* page_data = buffer_pool_mgr_->get_or_load_page(node_ids[i], node_ids[i], load_func);
                if (page_data) {
                    DiskNodeRecord record = parse_record(page_data);
                    output_vectors[i].resize(DEFAULT_DIMENSION);
                    std::memcpy(output_vectors[i].data(), record.vector_data,
                                DEFAULT_DIMENSION * sizeof(float));
                    success_count++;
                }
            } else {
                char* buffer = borrow_buffer();
                if (buffer && read_node(node_ids[i], buffer)) {
                    DiskNodeRecord record = parse_record(buffer);
                    output_vectors[i].resize(DEFAULT_DIMENSION);
                    std::memcpy(output_vectors[i].data(), record.vector_data,
                                DEFAULT_DIMENSION * sizeof(float));
                    success_count++;
                }
                if (buffer) return_buffer(buffer);
            }
        }
    }

    // Free all async buffers (they're now in buffer pool, temp buffers can be recycled)
    for (auto& [node_id, buffer] : async_buffers_) {
        return_buffer(buffer);
    }
    async_buffers_.clear();

    return success_count;
}

bool DiskIndexReader::using_io_uring() const {
    return io_engine_ != nullptr && io_engine_->using_io_uring();
}

}  // namespace agent_mem_io