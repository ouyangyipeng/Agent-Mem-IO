/**
 * @file disk_layout.cpp
 * @brief Disk-resident node record layout implementation
 *
 * Implements writing and reading 4KB-aligned node records for O_DIRECT SSD access.
 * Each record packs a vector + neighbor IDs + neighbor PQ codes into one 4KB block.
 */

#include "io/disk_layout.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <algorithm>

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
    posix_memalign(reinterpret_cast<void**>(&write_buffer), ALIGNMENT, DISK_RECORD_SIZE);

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
// DiskIndexReader Implementation
// =============================================================================

DiskIndexReader::DiskIndexReader(const std::string& data_dir,
                                  Size num_vectors,
                                  Size max_degree)
    : data_dir_(data_dir)
    , num_vectors_(num_vectors)
    , max_degree_(max_degree)
    , index_path_(data_dir + "/disk_index.bin")
    , fd_(-1) {
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
    return true;
}

void DiskIndexReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
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
    char* buffer = alloc_aligned_buffer();
    if (!buffer) return Vector();

    Vector vec(DEFAULT_DIMENSION, 0.0f);

    if (read_node(node_id, buffer)) {
        // Skip node_id (4 bytes), read vector starting at offset 4
        std::memcpy(vec.data(), buffer + sizeof(NodeId),
                    DEFAULT_DIMENSION * sizeof(float));
    }

    free(buffer);
    return vec;
}

Size DiskIndexReader::calculate_offset(NodeId node_id) const {
    // Each node record occupies exactly DISK_RECORD_SIZE (4KB) on disk
    return static_cast<Size>(node_id) * DISK_RECORD_SIZE;
}

char* DiskIndexReader::alloc_aligned_buffer() {
    char* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), ALIGNMENT, DISK_RECORD_SIZE);
    return buffer;
}

}  // namespace agent_mem_io