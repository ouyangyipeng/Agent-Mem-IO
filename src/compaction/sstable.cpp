/**
 * @file sstable.cpp
 * @brief SSTable file format utilities — validation, index building, and stats
 *
 * Provides SSTable-specific utility functions that complement SSTableManager
 * (which remains in memtable.cpp for tight coupling with MemTable flush logic):
 *   - SSTable file format validation
 *   - Sparse index construction for faster lookups
 *   - SSTable statistics reporting
 *
 * The SSTable binary format (magic "SSTABLE1"):
 *   [8 bytes: magic] [8 bytes: num_entries]
 *   For each entry:
 *     [4 bytes: node_id] [4 bytes: vec_size] [vec_size*4 bytes: vector] [1 byte: is_deleted]
 */

#include "compaction/memtable.h"
#include "common/types.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace agent_mem_io {

// =============================================================================
// SSTable File Format Validation
// =============================================================================

bool validate_sstable_file(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Read and verify magic header
    char magic[8];
    file.read(magic, 8);
    if (file.gcount() != 8 || std::memcmp(magic, "SSTABLE1", 8) != 0) {
        return false;
    }

    // Read entry count
    uint64_t num_entries = 0;
    file.read(reinterpret_cast<char*>(&num_entries), sizeof(num_entries));
    if (file.gcount() != sizeof(num_entries) || num_entries == 0) {
        return false;
    }

    // Verify we can read the first entry (format consistency check)
    NodeId first_node_id;
    file.read(reinterpret_cast<char*>(&first_node_id), sizeof(first_node_id));
    if (file.gcount() != sizeof(first_node_id)) {
        return false;
    }

    uint32_t vec_size;
    file.read(reinterpret_cast<char*>(&vec_size), sizeof(vec_size));
    if (file.gcount() != sizeof(vec_size) || vec_size == 0 || vec_size > 4096) {
        return false;  // Sanity check: dimension shouldn't exceed 4096
    }

    file.close();
    return true;
}

// =============================================================================
// SSTable Sparse Index Builder
// =============================================================================

struct SSTableSparseIndex {
    /// Every N-th entry is sampled to create a sparse index for faster lookups
    static constexpr Size SAMPLING_INTERVAL = 64;

    std::vector<NodeId> sampled_node_ids;   // Sorted node IDs at sampling points
    std::vector<Offset> sampled_offsets;    // File offsets for sampled entries
    NodeId min_node_id;
    NodeId max_node_id;
};

SSTableSparseIndex build_sparse_index(const std::string& filepath) {
    SSTableSparseIndex index;
    index.min_node_id = INVALID_NODE_ID;
    index.max_node_id = INVALID_NODE_ID;

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return index;
    }

    // Skip header (magic + num_entries = 16 bytes)
    file.seekg(16);

    uint64_t entry_count = 0;
    Offset current_offset = 16;

    while (file.good()) {
        NodeId node_id;
        file.read(reinterpret_cast<char*>(&node_id), sizeof(node_id));
        if (file.gcount() != sizeof(node_id)) break;

        uint32_t vec_size;
        file.read(reinterpret_cast<char*>(&vec_size), sizeof(vec_size));
        if (file.gcount() != sizeof(vec_size)) break;

        // Skip vector data
        file.seekg(vec_size * sizeof(float), std::ios::cur);

        // Skip is_deleted flag
        file.seekg(sizeof(bool), std::ios::cur);

        // Update min/max
        if (index.min_node_id == INVALID_NODE_ID || node_id < index.min_node_id) {
            index.min_node_id = node_id;
        }
        if (node_id > index.max_node_id) {
            index.max_node_id = node_id;
        }

        // Sample at interval
        if (entry_count % SSTableSparseIndex::SAMPLING_INTERVAL == 0) {
            index.sampled_node_ids.push_back(node_id);
            index.sampled_offsets.push_back(current_offset);
        }

        current_offset = static_cast<Offset>(file.tellg());
        entry_count++;
    }

    file.close();
    return index;
}

// =============================================================================
// SSTable Statistics
// =============================================================================

struct SSTableFileStats {
    Size num_entries;
    Size num_deleted;         // Tombstone markers
    Size file_size_bytes;
    NodeId min_node_id;
    NodeId max_node_id;
    float avg_vector_norm;    // Average L2 norm of stored vectors
};

SSTableFileStats compute_sstable_stats(const std::string& filepath) {
    SSTableFileStats stats{};
    stats.min_node_id = INVALID_NODE_ID;
    stats.max_node_id = INVALID_NODE_ID;

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return stats;
    }
    stats.file_size_bytes = static_cast<Size>(file.tellg());
    file.seekg(0);

    // Read header
    char magic[8];
    file.read(magic, 8);
    if (std::memcmp(magic, "SSTABLE1", 8) != 0) {
        return stats;
    }

    uint64_t num_entries;
    file.read(reinterpret_cast<char*>(&num_entries), sizeof(num_entries));
    stats.num_entries = static_cast<Size>(num_entries);

    float total_norm_sq = 0.0f;

    for (uint64_t i = 0; i < num_entries; ++i) {
        NodeId node_id;
        file.read(reinterpret_cast<char*>(&node_id), sizeof(node_id));

        uint32_t vec_size;
        file.read(reinterpret_cast<char*>(&vec_size), sizeof(vec_size));

        Vector vec(vec_size);
        if (vec_size > 0) {
            file.read(reinterpret_cast<char*>(vec.data()), vec_size * sizeof(float));
        }

        bool is_deleted;
        file.read(reinterpret_cast<char*>(&is_deleted), sizeof(is_deleted));

        if (is_deleted) {
            stats.num_deleted++;
        } else {
            // Compute norm
            float norm_sq = 0.0f;
            for (float v : vec) {
                norm_sq += v * v;
            }
            total_norm_sq += norm_sq;
        }

        if (node_id < stats.min_node_id || stats.min_node_id == INVALID_NODE_ID) {
            stats.min_node_id = node_id;
        }
        if (node_id > stats.max_node_id) {
            stats.max_node_id = node_id;
        }
    }

    Size live_entries = stats.num_entries - stats.num_deleted;
    stats.avg_vector_norm = (live_entries > 0)
        ? std::sqrt(total_norm_sq / static_cast<float>(live_entries)) : 0.0f;

    file.close();
    return stats;
}

}  // namespace agent_mem_io