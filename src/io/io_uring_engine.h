/**
 * @file io_uring_engine.h
 * @brief io_uring-based asynchronous I/O engine for vector data access
 * 
 * This file implements an asynchronous I/O engine using Linux io_uring
 * for high-performance vector data retrieval from SSD. The engine supports
 * batch submission and completion polling for maximum throughput.
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <system_error>

#if USE_IOURING
#include <liburing.h>
#endif  // USE_IOURING

namespace agent_mem_io {

/**
 * @brief I/O request structure
 */
struct IoRequest {
    int fd;                    // File descriptor
    Offset offset;             // File offset
    Size size;                 // I/O size
    char* buffer;              // Buffer for data
    NodeId node_id;            // Associated node ID (for callback)
    bool is_read;              // true for read, false for write
    uint64_t user_data;        // User data for completion callback
    
    IoRequest()
        : fd(-1)
        , offset(0)
        , size(0)
        , buffer(nullptr)
        , node_id(INVALID_NODE_ID)
        , is_read(true)
        , user_data(0)
    {}
};

/**
 * @brief I/O completion result
 */
struct IoCompletion {
    NodeId node_id;            // Associated node ID
    char* buffer;              // Buffer containing data
    Size size;                 // Bytes transferred
    int result;                // Result code (0 for success, negative for error)
    uint64_t user_data;        // User data from request
    bool is_read;              // true for read, false for write
    
    IoCompletion()
        : node_id(INVALID_NODE_ID)
        , buffer(nullptr)
        , size(0)
        , result(0)
        , user_data(0)
        , is_read(true)
    {}
    
    bool success() const { return result >= 0; }
};

/**
 * @brief I/O engine configuration
 */
struct IoEngineConfig {
    Size queue_depth = 256;           // io_uring queue depth
    Size batch_size = 32;             // Maximum batch size for submission
    bool use_polling = false;         // Use busy polling instead of interrupts
    bool use_fixed_files = false;     // Use fixed file descriptors
    bool use_fixed_buffers = false;   // Use fixed buffers
    int io_prio = 0;                  // I/O priority (0 = default, higher = lower priority)
    bool enable_sqpoll = false;       // Enable SQ polling thread
    bool enable_async = true;         // Enable async submission
    
    IoEngineConfig() = default;
};

/**
 * @brief I/O priority levels
 */
enum class IoPriority {
    HIGH = 0,      // Query I/O (real-time)
    NORMAL = 1,    // Default I/O
    LOW = 2,       // Compaction I/O (background)
    IDLE = 3       // Lowest priority
};

/**
 * @brief io_uring-based asynchronous I/O engine
 * 
 * Provides high-performance asynchronous I/O operations using Linux io_uring.
 * Supports batch submission, completion polling, and I/O priority control.
 */
class IoUringEngine {
public:
    /**
     * @brief Construct io_uring engine
     * @param config Engine configuration
     */
    explicit IoUringEngine(const IoEngineConfig& config);
    
    /**
     * @brief Destructor
     */
    ~IoUringEngine();
    
    /**
     * @brief Initialize the engine
     * @return Error status
     */
    Error init();
    
    /**
     * @brief Shutdown the engine
     */
    void shutdown();
    
    /**
     * @brief Submit a single I/O request
     * @param request I/O request
     * @return Error status
     */
    Error submit(const IoRequest& request);
    
    /**
     * @brief Submit multiple I/O requests in batch
     * @param requests List of I/O requests
     * @return Number of requests submitted
     */
    Size submit_batch(const std::vector<IoRequest>& requests);
    
    /**
     * @brief Submit read request
     * @param fd File descriptor
     * @param offset File offset
     * @param buffer Destination buffer
     * @param size Read size
     * @param node_id Associated node ID
     * @param priority I/O priority
     * @return Error status
     */
    Error submit_read(
        int fd,
        Offset offset,
        char* buffer,
        Size size,
        NodeId node_id,
        IoPriority priority = IoPriority::NORMAL
    );
    
    /**
     * @brief Submit write request
     * @param fd File descriptor
     * @param offset File offset
     * @param buffer Source buffer
     * @param size Write size
     * @param node_id Associated node ID
     * @param priority I/O priority
     * @return Error status
     */
    Error submit_write(
        int fd,
        Offset offset,
        const char* buffer,
        Size size,
        NodeId node_id,
        IoPriority priority = IoPriority::NORMAL
    );
    
    /**
     * @brief Wait for a single completion
     * @param timeout_ms Timeout in milliseconds (-1 for infinite)
     * @return I/O completion result
     */
    IoCompletion wait_completion(int timeout_ms = -1);
    
    /**
     * @brief Wait for multiple completions
     * @param max_completions Maximum number of completions to wait for
     * @param timeout_ms Timeout in milliseconds (-1 for infinite)
     * @return List of I/O completion results
     */
    std::vector<IoCompletion> wait_completion_batch(
        Size max_completions,
        int timeout_ms = -1
    );
    
    /**
     * @brief Poll for completions (non-blocking)
     * @return List of available completions (may be empty)
     */
    std::vector<IoCompletion> poll_completions();
    
    /**
     * @brief Get number of pending requests
     * @return Number of pending requests in submission queue
     */
    Size pending_count() const;
    
    /**
     * @brief Get number of completed requests
     * @return Number of completed requests in completion queue
     */
    Size completed_count() const;
    
    /**
     * @brief Check if engine is initialized
     * @return true if initialized
     */
    bool is_initialized() const;
    
    /**
     * @brief Get queue depth
     * @return Queue depth
     */
    Size queue_depth() const;
    
    /**
     * @brief Register file descriptor for fixed file support
     * @param fd File descriptor
     * @return Index of registered file, or -1 on error
     */
    int register_file(int fd);
    
    /**
     * @brief Unregister file descriptor
     * @param index Index of registered file
     * @return Error status
     */
    Error unregister_file(int index);
    
    /**
     * @brief Register buffer for fixed buffer support
     * @param buffer Buffer pointer
     * @param size Buffer size
     * @return Index of registered buffer, or -1 on error
     */
    int register_buffer(char* buffer, Size size);
    
    /**
     * @brief Unregister buffer
     * @param index Index of registered buffer
     * @return Error status
     */
    Error unregister_buffer(int index);

private:
#if USE_IOURING
    /**
     * @brief Prepare read SQE
     * @param sqe Submission queue entry
     * @param request I/O request
     * @param priority I/O priority
     */
    void prepare_read_sqe(struct io_uring_sqe* sqe, const IoRequest& request, IoPriority priority);
    
    /**
     * @brief Prepare write SQE
     * @param sqe Submission queue entry
     * @param request I/O request
     * @param priority I/O priority
     */
    void prepare_write_sqe(struct io_uring_sqe* sqe, const IoRequest& request, IoPriority priority);
    
    /**
     * @brief Process completion queue entry
     * @param cqe Completion queue entry
     * @return I/O completion result
     */
    IoCompletion process_cqe(struct io_uring_cqe* cqe);
    
    struct io_uring ring_;
#endif  // USE_IOURING
    
    IoEngineConfig config_;
    bool initialized_;
    
    // Registered files and buffers
    std::vector<int> registered_files_;
    std::vector<std::pair<char*, Size>> registered_buffers_;
    
    // Statistics
    std::atomic<uint64_t> total_submitted_{0};
    std::atomic<uint64_t> total_completed_{0};
    std::atomic<uint64_t> total_errors_{0};
    
    mutable std::mutex mutex_;
};

/**
 * @brief Fallback synchronous I/O engine (for systems without io_uring)
 * 
 * Provides synchronous I/O operations using pread/pwrite with O_DIRECT.
 * Used as fallback when io_uring is not available.
 */
class SyncIoEngine {
public:
    /**
     * @brief Construct synchronous I/O engine
     */
    SyncIoEngine();
    
    /**
     * @brief Destructor
     */
    ~SyncIoEngine();
    
    /**
     * @brief Read from file
     * @param fd File descriptor
     * @param offset File offset
     * @param buffer Destination buffer
     * @param size Read size
     * @return Bytes read, or negative on error
     */
    int read(int fd, Offset offset, char* buffer, Size size);
    
    /**
     * @brief Write to file
     * @param fd File descriptor
     * @param offset File offset
     * @param buffer Source buffer
     * @param size Write size
     * @return Bytes written, or negative on error
     */
    int write(int fd, Offset offset, const char* buffer, Size size);
    
    /**
     * @brief Open file with O_DIRECT
     * @param filepath File path
     * @param read_only Whether to open read-only
     * @return File descriptor, or -1 on error
     */
    int open_file(const std::string& filepath, bool read_only = false);
    
    /**
     * @brief Close file
     * @param fd File descriptor
     */
    void close_file(int fd);
};

/**
 * @brief Unified I/O engine interface
 * 
 * Provides a unified interface that uses io_uring when available,
 * or falls back to synchronous I/O otherwise.
 */
class IoEngine {
public:
    /**
     * @brief Construct I/O engine
     * @param config Engine configuration
     */
    explicit IoEngine(const IoEngineConfig& config = IoEngineConfig());
    
    /**
     * @brief Destructor
     */
    ~IoEngine();
    
    /**
     * @brief Initialize the engine
     * @return Error status
     */
    Error init();
    
    /**
     * @brief Shutdown the engine
     */
    void shutdown();
    
    /**
     * @brief Submit read request
     * @param fd File descriptor
     * @param offset File offset
     * @param buffer Destination buffer
     * @param size Read size
     * @param node_id Associated node ID
     * @param priority I/O priority
     * @return Error status
     */
    Error submit_read(
        int fd,
        Offset offset,
        char* buffer,
        Size size,
        NodeId node_id,
        IoPriority priority = IoPriority::NORMAL
    );
    
    /**
     * @brief Submit write request
     * @param fd File descriptor
     * @param offset File offset
     * @param buffer Source buffer
     * @param size Write size
     * @param node_id Associated node ID
     * @param priority I/O priority
     * @return Error status
     */
    Error submit_write(
        int fd,
        Offset offset,
        const char* buffer,
        Size size,
        NodeId node_id,
        IoPriority priority = IoPriority::NORMAL
    );
    
    /**
     * @brief Submit batch read requests
     * @param requests List of I/O requests
     * @return Number of requests submitted
     */
    Size submit_batch_read(const std::vector<IoRequest>& requests);
    
    /**
     * @brief Wait for completion
     * @param timeout_ms Timeout in milliseconds
     * @return I/O completion result
     */
    IoCompletion wait_completion(int timeout_ms = -1);
    
    /**
     * @brief Wait for batch completions
     * @param max_completions Maximum completions
     * @param timeout_ms Timeout in milliseconds
     * @return List of completions
     */
    std::vector<IoCompletion> wait_completion_batch(Size max_completions, int timeout_ms = -1);
    
    /**
     * @brief Check if using io_uring
     * @return true if using io_uring
     */
    bool using_io_uring() const;
    
    /**
     * @brief Check if initialized
     * @return true if initialized
     */
    bool is_initialized() const;

private:
#if USE_IOURING
    std::unique_ptr<IoUringEngine> io_uring_engine_;
#endif  // USE_IOURING
    std::unique_ptr<SyncIoEngine> sync_engine_;
    bool use_io_uring_;
    bool initialized_;
};

}  // namespace agent_mem_io