/**
 * @file types.h
 * @brief Common type definitions for Agent-Mem-IO storage engine
 * 
 * This file defines the core types used throughout the storage engine,
 * including node IDs, vector types, distance types, and configuration
 * constants.
 */

#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <stdexcept>

namespace agent_mem_io {

// =============================================================================
// Basic Types
// =============================================================================

/// Node identifier type (uint32_t for up to 4 billion nodes)
using NodeId = uint32_t;

/// Invalid node ID constant
constexpr NodeId INVALID_NODE_ID = static_cast<NodeId>(-1);

/// Vector dimension type
using Dimension = uint32_t;

/// Default vector dimension (SIFT dataset uses 128)
constexpr Dimension DEFAULT_DIMENSION = 128;

/// Vector data type (float for most vector datasets)
using VectorValue = float;

/// Vector type (std::vector of floats)
using Vector = std::vector<VectorValue>;

/// Distance type (float for L2 distance, cosine similarity, etc.)
using Distance = float;

/// Neighbor type (pair of node ID and distance)
using Neighbor = std::pair<NodeId, Distance>;

/// Neighbor list type
using NeighborList = std::vector<Neighbor>;

/// Node degree type
using Degree = uint32_t;

/// Page ID type for buffer pool
using PageId = uint64_t;

/// Invalid page ID constant
constexpr PageId INVALID_PAGE_ID = static_cast<PageId>(-1);

/// Offset type for file I/O
using Offset = uint64_t;

/// Size type
using Size = uint64_t;

// =============================================================================
// Configuration Constants
// =============================================================================

/// Default page size (4KB, aligned for O_DIRECT)
constexpr Size PAGE_SIZE = 4096;

/// Default alignment for O_DIRECT buffers
constexpr Size ALIGNMENT = 4096;

/// Vector size in bytes (128 dimensions × 4 bytes per float)
constexpr Size VECTOR_SIZE_BYTES = DEFAULT_DIMENSION * sizeof(VectorValue);

/// Default maximum degree for graph index
constexpr Degree DEFAULT_MAX_DEGREE = 64;

/// Default search list size for Vamana algorithm
constexpr Size DEFAULT_SEARCH_LIST_SIZE = 100;

/// Default buffer pool size (in pages)
constexpr Size DEFAULT_BUFFER_POOL_PAGES = 10000;

/// Default memtable size limit (in bytes)
constexpr Size DEFAULT_MEMTABLE_SIZE = 10 * 1024 * 1024;  // 10 MB

/// Default WAL file name
const std::string DEFAULT_WAL_FILE = "wal.log";

/// Default vector data file name
const std::string DEFAULT_VECTOR_FILE = "vectors.bin";

/// Default graph index file name
const std::string DEFAULT_GRAPH_FILE = "graph.bin";

// =============================================================================
// Memory Budget Constants (for SIFT1M dataset)
// =============================================================================

/// SIFT1M dataset size (1 million vectors × 128 dimensions × 4 bytes)
constexpr Size SIFT1M_DATASET_SIZE = 1000000 * DEFAULT_DIMENSION * sizeof(VectorValue);

/// Memory limit ratio (10% of dataset size)
constexpr double MEMORY_LIMIT_RATIO_MIN = 0.10;

/// Memory limit ratio (20% of dataset size)
constexpr double MEMORY_LIMIT_RATIO_MAX = 0.20;

/// Minimum recall requirement
constexpr double MIN_RECALL_AT_10 = 0.85;

// =============================================================================
// Result Types
// =============================================================================

/// Search result type
struct SearchResult {
    NodeId node_id;
    Distance distance;
    
    SearchResult() : node_id(INVALID_NODE_ID), distance(0.0f) {}
    SearchResult(NodeId id, Distance dist) : node_id(id), distance(dist) {}
    
    bool operator<(const SearchResult& other) const {
        return distance < other.distance;
    }
    
    bool operator>(const SearchResult& other) const {
        return distance > other.distance;
    }
};

/// Search results list (Top-K results)
using SearchResults = std::vector<SearchResult>;

/// Insert result type
struct InsertResult {
    bool success;
    NodeId node_id;
    std::string error_message;
    
    InsertResult() : success(false), node_id(INVALID_NODE_ID), error_message("") {}
    InsertResult(bool s, NodeId id) : success(s), node_id(id), error_message("") {}
    InsertResult(bool s, NodeId id, const std::string& err) 
        : success(s), node_id(id), error_message(err) {}
};

/// Query result type
struct QueryResult {
    SearchResults results;
    double latency_ms;
    size_t io_count;
    double cache_hit_rate;
    
    QueryResult() : latency_ms(0.0), io_count(0), cache_hit_rate(0.0) {}
};

// =============================================================================
// Error Types
// =============================================================================

/// Error codes
enum class ErrorCode {
    SUCCESS = 0,
    INVALID_ARGUMENT,
    IO_ERROR,
    MEMORY_LIMIT_EXCEEDED,
    NODE_NOT_FOUND,
    GRAPH_BUILD_FAILED,
    COMPACTION_FAILED,
    WAL_ERROR,
    BUFFER_POOL_FULL,
    UNKNOWN_ERROR
};

/// Error class
class Error {
public:
    Error(ErrorCode code, const std::string& message)
        : code_(code), message_(message) {}
    
    ErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }
    
    bool ok() const { return code_ == ErrorCode::SUCCESS; }
    
    static Error success() { return Error(ErrorCode::SUCCESS, ""); }
    static Error invalid_argument(const std::string& msg) {
        return Error(ErrorCode::INVALID_ARGUMENT, msg);
    }
    static Error io_error(const std::string& msg) {
        return Error(ErrorCode::IO_ERROR, msg);
    }
    static Error memory_limit_exceeded(const std::string& msg) {
        return Error(ErrorCode::MEMORY_LIMIT_EXCEEDED, msg);
    }
    static Error node_not_found(const std::string& msg) {
        return Error(ErrorCode::NODE_NOT_FOUND, msg);
    }
    static Error buffer_pool_full(const std::string& msg) {
        return Error(ErrorCode::BUFFER_POOL_FULL, msg);
    }

private:
    ErrorCode code_;
    std::string message_;
};

/// Exception class for storage engine errors
class StorageEngineException : public std::runtime_error {
public:
    StorageEngineException(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    
    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

// =============================================================================
// Callback Types
// =============================================================================

/// I/O completion callback type
using IoCompletionCallback = std::function<void(NodeId, const Vector&, const Error&)>;

/// Search completion callback type
using SearchCompletionCallback = std::function<void(const SearchResults&, const Error&)>;

/// Compaction completion callback type
using CompactionCallback = std::function<void(bool success, const std::string& message)>;

// =============================================================================
// Utility Functions
// =============================================================================

/// Calculate vector offset in file (aligned to page size)
inline Offset calculate_vector_offset(NodeId node_id) {
    // Each vector is stored in a separate page for O_DIRECT access
    return static_cast<Offset>(node_id) * PAGE_SIZE;
}

/// Calculate page ID from node ID
inline PageId calculate_page_id(NodeId node_id) {
    return static_cast<PageId>(node_id);
}

/// Check if memory usage is within limits
inline bool is_memory_within_limits(Size current_memory, Size dataset_size) {
    double ratio = static_cast<double>(current_memory) / static_cast<double>(dataset_size);
    return ratio >= MEMORY_LIMIT_RATIO_MIN && ratio <= MEMORY_LIMIT_RATIO_MAX;
}

/// Calculate L2 distance between two vectors
Distance l2_distance(const Vector& v1, const Vector& v2);

/// Calculate inner product distance between two vectors
Distance inner_product_distance(const Vector& v1, const Vector& v2);

/// Calculate cosine distance between two vectors
Distance cosine_distance(const Vector& v1, const Vector& v2);

}  // namespace agent_mem_io