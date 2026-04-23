/**
 * @file disk_layout.cpp
 * @brief Disk-resident node record layout implementation
 *
 * Implements writing and reading 4KB-aligned node records for O_DIRECT SSD access.
 * Enhanced with:
 *   - BufferPoolManager: Graph-Aware 2Q eviction (hub node protection)
 *   - Buffer pool: pre-allocated aligned buffers (eliminates alloc/dealloc overhead)
 *   - Batch reads: preadv for multiple nodes in fewer syscalls
 *   - io_uring fixed file registration for reduced syscall overhead
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

    // Write node ID at offset 0, then padding to 32 bytes
    // This ensures vector data starts at offset 32 (AVX2-aligned within 4KB page:
    // 4KB-aligned page base + 32 = 32-byte aligned for _mm256_load_ps)
    std::memcpy(buffer, &node_id, sizeof(NodeId));
    // Padding bytes 4-31 are already zeroed by memset above

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
    // Free all temp I/O buffer pool entries
    for (char* buf : io_buffer_pool_) {
        free(buf);
    }
    io_buffer_pool_.clear();
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

    // Initialize I/O buffer pool with pre-allocated aligned buffers
    init_io_buffer_pool(io_buffer_pool_capacity_);

    return true;
}

void DiskIndexReader::close() {
    // Unregister io_uring fixed buffers before closing file
    if (io_engine_ && !registered_buffer_indices_.empty()) {
        for (int idx : registered_buffer_indices_) {
            io_engine_->unregister_buffer(idx);
        }
        registered_buffer_indices_.clear();
        std::cout << "[DiskIndexReader] Unregistered io_uring fixed buffers\n";
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void DiskIndexReader::init_io_buffer_pool(Size pool_size) {
    for (Size i = 0; i < pool_size; ++i) {
        char* buf = alloc_aligned_buffer();
        if (buf) {
            io_buffer_pool_.push_back(buf);
        }
    }
}

char* DiskIndexReader::borrow_buffer() {
    std::lock_guard<std::mutex> lock(io_buffer_pool_mutex_);
    if (!io_buffer_pool_.empty()) {
        char* buf = io_buffer_pool_.back();
        io_buffer_pool_.pop_back();
        return buf;
    }

    // Pool exhausted: allocate a new buffer (will be returned to pool later)
    // Note: alloc_aligned_buffer() is static and thread-safe (posix_memalign)
    char* buf = alloc_aligned_buffer();
    return buf;
}

void DiskIndexReader::return_buffer(char* buffer) {
    if (buffer) {
        std::lock_guard<std::mutex> lock(io_buffer_pool_mutex_);
        io_buffer_pool_.push_back(buffer);
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

    // Read node ID at offset 0 (header is 32 bytes: 4B node_id + 28B padding)
    std::memcpy(&record.node_id, buffer, sizeof(NodeId));

    // Read full-precision vector at aligned offset 32 (AVX2 32-byte aligned)
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

void DiskIndexReader::update_buffer_pool_hub_threshold(
    const std::vector<uint32_t>& in_degrees) {
    if (!buffer_pool_mgr_) return;

    // Compute dynamic hub threshold from in-degree distribution
    // and update the eviction policy to protect hub nodes
    buffer_pool_mgr_->update_hub_threshold(in_degrees);
    std::cout << "[DiskIndexReader] Updated dynamic hub threshold for "
              << "Graph-Aware 2Q eviction (hub nodes protected)\n";
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
    // Thread-safe distance computation using atomic get+pin pattern.
    //
    // CRITICAL: Under multi-threaded search, the old get_page() → SIMD → done
    // sequence had a data race: get_page() releases the shared lock, then
    // another thread's 2Q eviction could free the page buffer while SIMD is
    // still reading from it. The atomic get_and_pin_page() / get_or_load_and_pin_page()
    // acquire a write lock and pin the page before returning the pointer,
    // guaranteeing the buffer remains valid throughout the SIMD computation.
    //
    // After computing the distance, we immediately unpin (micro-level unpin)
    // so the page can be evicted again. The search-level pin (pinned_pages)
    // in diskann_search_enhanced() provides longer-lived protection.
    if (buffer_pool_mgr_) {
        // Fast path: page is cached → get_and_pin atomically, compute, unpin
        char* page_data = buffer_pool_mgr_->get_and_pin_page(node_id, node_id);
        if (page_data) {
            const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
            float dist = l2_distance_sq_simd(query.data(), vec_ptr, query.size());
            // Micro-level unpin: page is safe to evict again after SIMD read completes
            buffer_pool_mgr_->unpin_page(node_id);
            return dist;
        }

        // Slow path: not cached → load+pin atomically, compute, unpin
        auto load_func = [this](char* buffer, PageId pid) {
            Size offset = calculate_offset(static_cast<NodeId>(pid));
            ssize_t bytes_read = pread(fd_, buffer, DISK_RECORD_SIZE, offset);
            if (bytes_read != DISK_RECORD_SIZE) return;
        };
        page_data = buffer_pool_mgr_->get_or_load_and_pin_page(node_id, node_id, load_func);
        if (page_data) {
            const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
            float dist = l2_distance_sq_simd(query.data(), vec_ptr, query.size());
            buffer_pool_mgr_->unpin_page(node_id);
            return dist;
        }
    }

    // No BufferPool: fall back to read_vector + manual distance
    Vector vec = read_vector(node_id);
    if (vec.empty()) return -1.0f;
    return l2_distance_sq_simd(query.data(), vec.data(), query.size());
}

bool DiskIndexReader::ensure_page_pinned(NodeId node_id) {
    if (!buffer_pool_mgr_) return false;

    // Fast path: page is already cached → just pin it
    if (buffer_pool_mgr_->pin_page(node_id)) {
        return true;
    }

    // Slow path: page not in cache → load+pin atomically
    // This ensures the page is in the cache AND pinned before we compute
    // distance from it. Without this, compute_distance_direct() would
    // load the page (micro-level pin/unpin), but another thread could
    // evict it before our search-level pin_page() call.
    auto load_func = [this](char* buffer, PageId pid) {
        Size offset = calculate_offset(static_cast<NodeId>(pid));
        ssize_t bytes_read = pread(fd_, buffer, DISK_RECORD_SIZE, offset);
        if (bytes_read != DISK_RECORD_SIZE) return;
    };
    char* page_data = buffer_pool_mgr_->get_or_load_and_pin_page(node_id, node_id, load_func);
    if (page_data) {
        // get_or_load_and_pin_page already pinned it (pin_count=1).
        // We want search-level pin (pin_count=2) so that after
        // compute_distance_direct()'s micro-level unpin (pin_count→1),
        // the page is still pinned at pin_count=1.
        // But get_or_load_and_pin_page already set pin_count=1,
        // and compute_distance_direct will do pin→2, compute, unpin→1.
        // So the page stays at pin_count=1 after micro-level unpin. ✓
        return true;
    }

    return false;
}

bool DiskIndexReader::pin_page(NodeId node_id) {
    if (!buffer_pool_mgr_) return false;
    return buffer_pool_mgr_->pin_page(node_id);
}

bool DiskIndexReader::unpin_page(NodeId node_id) {
    if (!buffer_pool_mgr_) return false;
    return buffer_pool_mgr_->unpin_page(node_id);
}

void DiskIndexReader::unpin_all_pages(const std::vector<NodeId>& node_ids) {
    if (!buffer_pool_mgr_) return;
    // Convert NodeId vector to PageId vector for BufferPoolManager
    std::vector<PageId> page_ids(node_ids.size());
    for (Size i = 0; i < node_ids.size(); ++i) {
        page_ids[i] = static_cast<PageId>(node_ids[i]);
    }
    buffer_pool_mgr_->unpin_all_pages(page_ids);
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

    // Thread-safe batch distance computation using atomic get+pin+unpin pattern.
    // Each distance computation: get_and_pin → SIMD → unpin (micro-level protection).
    // This prevents the data race where another thread evicts the page between
    // get_page() and the SIMD distance computation.
    for (Size i = 0; i < node_ids.size(); ++i) {
        // Software prefetch for next vector to improve cache utilization
        if (i + 1 < node_ids.size()) {
            if (buffer_pool_mgr_->contains(node_ids[i + 1])) {
                char* next_page = buffer_pool_mgr_->get_page(node_ids[i + 1], node_ids[i + 1]);
                if (next_page) {
                    __builtin_prefetch(next_page + DISK_VECTOR_OFFSET, 0, 1);
                }
            }
        }

        // Fast path: cached → get_and_pin atomically, compute, unpin
        char* page_data = buffer_pool_mgr_->get_and_pin_page(node_ids[i], node_ids[i]);
        if (page_data) {
            const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
            distances[i] = l2_distance_sq_simd(query.data(), vec_ptr, query.size());
            buffer_pool_mgr_->unpin_page(node_ids[i]);
            success_count++;
            continue;
        }

        // Slow path: not cached → load+pin atomically, compute, unpin
        auto load_func = [this](char* buffer, PageId pid) {
            Size offset = calculate_offset(static_cast<NodeId>(pid));
            ssize_t bytes_read = pread(fd_, buffer, DISK_RECORD_SIZE, offset);
            if (bytes_read != DISK_RECORD_SIZE) return;
        };
        page_data = buffer_pool_mgr_->get_or_load_and_pin_page(node_ids[i], node_ids[i], load_func);
        if (page_data) {
            const float* vec_ptr = reinterpret_cast<const float*>(page_data + DISK_VECTOR_OFFSET);
            distances[i] = l2_distance_sq_simd(query.data(), vec_ptr, query.size());
            buffer_pool_mgr_->unpin_page(node_ids[i]);
            success_count++;
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

        // Register the index file fd with io_uring for fixed-file optimization
        // This eliminates the per-submit syscall overhead of passing fd to the kernel
        if (fd_ >= 0 && io_engine_->using_io_uring() && io_engine_->is_initialized()) {
            int file_index = io_engine_->register_file(fd_);
            if (file_index >= 0) {
                std::cout << "[DiskIndexReader] Registered index file with io_uring "
                          << "(fixed-file index=" << file_index << ")\n";
            } else {
                std::cout << "[DiskIndexReader] io_uring file registration failed "
                          << "(will use regular fd submission)\n";
            }
        }

        // Register pre-allocated I/O buffers with io_uring for fixed-buffer optimization
        // This eliminates kernel copy overhead for O_DIRECT aligned buffers.
        // io_buffer_pool_ is already populated by init_io_buffer_pool() in open(),
        // so all buffers are available at this point.
        if (io_engine_->using_io_uring() && io_engine_->is_initialized()) {
            for (char* buf : io_buffer_pool_) {
                int buf_index = io_engine_->register_buffer(buf, DISK_RECORD_SIZE);
                if (buf_index >= 0) {
                    registered_buffer_indices_.push_back(buf_index);
                }
            }
            if (!registered_buffer_indices_.empty()) {
                std::cout << "[DiskIndexReader] Registered " << registered_buffer_indices_.size()
                          << " I/O buffers with io_uring (fixed-buffer optimization)\n";
            } else {
                std::cout << "[DiskIndexReader] io_uring buffer registration failed "
                          << "(will use regular buffer submission)\n";
            }
        }
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

        // Store buffer mapping for later retrieval (thread-safe)
        {
            std::lock_guard<std::mutex> lock(async_buffers_mutex_);
            async_buffers_[node_id] = buffer;
        }

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
    // Thread-safe design: async_buffers_ protected by async_buffers_mutex_.
    // Lock ordering constraint: never hold async_buffers_mutex_ while calling
    // return_buffer() (which takes io_buffer_pool_mutex_), to avoid deadlock.
    // In multi-threaded mode, we may reap completions submitted by other threads.
    // We insert ALL completed data into buffer_pool (shared, thread-safe) so
    // any thread can find it via cache hit later, even if the completion was
    // for a node_id not in our own node_ids list.
    Size max_completions = 0;
    {
        std::lock_guard<std::mutex> lock(async_buffers_mutex_);
        if (io_engine_ && !async_buffers_.empty()) {
            max_completions = async_buffers_.size();
        }
    }

    if (max_completions > 0) {
        auto completions = io_engine_->wait_completion_batch(max_completions, 100);

        // Collect buffers to return AFTER processing (outside async_buffers_mutex_)
        std::vector<char*> processed_buffers;

        for (const auto& comp : completions) {
            if (comp.success() && comp.node_id != INVALID_NODE_ID) {
                // Extract buffer from async_buffers_ under lock
                char* comp_buffer = nullptr;
                {
                    std::lock_guard<std::mutex> lock(async_buffers_mutex_);
                    auto buf_it = async_buffers_.find(comp.node_id);
                    if (buf_it != async_buffers_.end()) {
                        comp_buffer = buf_it->second;
                        async_buffers_.erase(buf_it);
                    }
                }

                if (!comp_buffer) continue;

                // Find index in our node_ids (may be -1 for another thread's completion)
                Size idx = static_cast<Size>(-1);
                for (Size j = 0; j < node_ids.size(); ++j) {
                    if (node_ids[j] == comp.node_id) {
                        idx = j;
                        break;
                    }
                }

                // ALWAYS insert into buffer_pool (shared cache, helps all threads)
                // This is critical for multi-threaded io_uring: when Thread A reaps
                // Thread B's completions, inserting into buffer_pool ensures Thread B
                // can find the data via cache hit in Phase 1 or Phase 3.
                if (buffer_pool_mgr_) {
                    buffer_pool_mgr_->put_page_data(comp.node_id, comp.node_id,
                                                     comp_buffer, DISK_RECORD_SIZE);
                    // Only populate output_vectors for our own node_ids
                    if (idx != static_cast<Size>(-1)) {
                        char* cached_data = buffer_pool_mgr_->get_page(comp.node_id, comp.node_id);
                        if (cached_data) {
                            DiskNodeRecord record = parse_record(cached_data);
                            output_vectors[idx].resize(DEFAULT_DIMENSION);
                            std::memcpy(output_vectors[idx].data(), record.vector_data,
                                        DEFAULT_DIMENSION * sizeof(float));
                            success_count++;
                        }
                    }
                } else if (idx != static_cast<Size>(-1)) {
                    // No buffer pool: only parse for our own nodes
                    DiskNodeRecord record = parse_record(comp_buffer);
                    output_vectors[idx].resize(DEFAULT_DIMENSION);
                    std::memcpy(output_vectors[idx].data(), record.vector_data,
                                DEFAULT_DIMENSION * sizeof(float));
                    success_count++;
                }

                processed_buffers.push_back(comp_buffer);
            }
        }

        // Return processed temp buffers to pool (outside async_buffers_mutex_)
        for (char* buf : processed_buffers) {
            return_buffer(buf);
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

    // Free remaining async buffers (completions not received — timed out or failed)
    // These are entries still in async_buffers_ after Phase 2 processing.
    // Lock protects async_buffers_ access; return_buffer() is called outside lock
    // to avoid deadlock (return_buffer takes io_buffer_pool_mutex_).
    std::vector<char*> remaining_buffers;
    {
        std::lock_guard<std::mutex> lock(async_buffers_mutex_);
        for (auto& [node_id, buffer] : async_buffers_) {
            remaining_buffers.push_back(buffer);
        }
        async_buffers_.clear();
    }
    for (char* buf : remaining_buffers) {
        return_buffer(buf);
    }

    return success_count;
}

bool DiskIndexReader::using_io_uring() const {
    return io_engine_ != nullptr && io_engine_->using_io_uring();
}

}  // namespace agent_mem_io