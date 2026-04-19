/**
 * @file disk_layout.h
 * @brief Disk-resident node record layout for DiskANN-style SSD storage
 *
 * Implements the Starling/DiskANN disk layout optimization where each node
 * is stored as a fixed-size 4KB record containing:
 *   - Node ID
 *   - Full-precision vector (128-dim × 4B = 512B)
 *   - Neighbor IDs (max_degree × 4B)
 *   - Neighbor PQ codes (max_degree × m bytes)
 *   - Padding to 4KB alignment
 *
 * Key benefit: ONE SSD read (4KB) gives us:
 *   1. The full-precision vector for exact distance computation
 *   2. Neighbor IDs for graph traversal continuation
 *   3. Neighbor PQ codes for ADC distance estimation WITHOUT additional I/O
 *
 * Without this layout, DiskANN wastes 94% of each 4KB I/O block (Starling paper).
 */

#pragma once

#include "common/types.h"
#include "core/pq_encoder.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

namespace agent_mem_io {

// =============================================================================
// Constants
// =============================================================================

/// Default maximum degree for disk node records
constexpr Size DEFAULT_DISK_MAX_DEGREE = 32;

/// Disk node record size (must be PAGE_SIZE = 4KB aligned for O_DIRECT)
constexpr Size DISK_RECORD_SIZE = PAGE_SIZE;  // 4096 bytes

/// Header size within a disk record
constexpr Size DISK_RECORD_HEADER_SIZE = sizeof(NodeId);  // 4 bytes

/// Vector data size within a disk record (128 × 4 = 512 bytes)
constexpr Size DISK_VECTOR_DATA_SIZE = DEFAULT_DIMENSION * sizeof(float);

// =============================================================================
// Disk Node Record
// =============================================================================

/**
 * @brief Fixed-size 4KB disk node record
 *
 * Layout within a 4096-byte record:
 *
 * Offset  Content                     Size
 * 0       Node ID                     4 bytes
 * 4       Full-precision vector       512 bytes (128 × 4B)
 * 516     Number of neighbors          4 bytes
 * 520     Neighbor IDs                max_degree × 4B (128B for R=32)
 * 648     Neighbor PQ codes           max_degree × 8B (256B for R=32, m=8)
 * 904     Padding to 4096B            remaining bytes
 *
 * For R=32, m=8: used = 4+512+4+128+256 = 904B, wasted = 3192B
 * Still ~22% utilization, but the key is we get all we need in ONE I/O.
 *
 * For R=64, m=8: used = 4+512+4+256+512 = 1288B, ~31% utilization
 * Much better than DiskANN's ~6% utilization (separate vector+graph files).
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
};

// =============================================================================
// Disk Index Writer
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
// Disk Index Reader
// =============================================================================

/**
 * @brief Reads node records from SSD using O_DIRECT
 *
 * Supports both synchronous reads (for testing) and batch async reads
 * (for beam-style search with io_uring).
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
     * @brief Open the index file with O_DIRECT
     * @return true if opened successfully
     */
    bool open();

    /**
     * @brief Close the index file
     */
    void close();

    /**
     * @brief Read a single node record from SSD (synchronous, O_DIRECT)
     * @param node_id Node to read
     * @param buffer 4KB-aligned buffer (must be posix_memalign'd)
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
     * @brief Read a node's full-precision vector from SSD
     * @param node_id Node to read
     * @return Vector data
     */
    Vector read_vector(NodeId node_id);

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

    /**
     * @brief Allocate 4KB-aligned buffer for O_DIRECT read
     */
    static char* alloc_aligned_buffer();
};

}  // namespace agent_mem_io