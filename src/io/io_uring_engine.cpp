/**
 * @file io_uring_engine.cpp
 * @brief io_uring async I/O engine implementation
 */

#include "io/io_uring_engine.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <system_error>

namespace agent_mem_io {

// =============================================================================
// SyncIoEngine Implementation
// =============================================================================

SyncIoEngine::SyncIoEngine() {}

SyncIoEngine::~SyncIoEngine() {}

int SyncIoEngine::read(int fd, Offset offset, char* buffer, Size size) {
    return static_cast<int>(::pread(fd, buffer, size, offset));
}

int SyncIoEngine::write(int fd, Offset offset, const char* buffer, Size size) {
    return static_cast<int>(::pwrite(fd, buffer, size, offset));
}

int SyncIoEngine::open_file(const std::string& filepath, bool read_only) {
    int flags = O_DIRECT;
    if (read_only) {
        flags |= O_RDONLY;
    } else {
        flags |= O_RDWR | O_CREAT;
    }
    
    int fd = ::open(filepath.c_str(), flags, 0644);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

void SyncIoEngine::close_file(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

// =============================================================================
// IoEngine Implementation
// =============================================================================

IoEngine::IoEngine(const IoEngineConfig& config)
    : use_io_uring_(false)
    , initialized_(false)
{
#if USE_IOURING
    io_uring_engine_ = std::make_unique<IoUringEngine>(config);
    use_io_uring_ = true;
#endif  // USE_IOURING
    sync_engine_ = std::make_unique<SyncIoEngine>();
}

IoEngine::~IoEngine() {
    shutdown();
}

Error IoEngine::init() {
#if USE_IOURING
    if (use_io_uring_ && io_uring_engine_) {
        Error err = io_uring_engine_->init();
        if (err.ok()) {
            initialized_ = true;
            return err;
        }
        // Fall back to sync I/O
        use_io_uring_ = false;
    }
#endif  // USE_IOURING
    initialized_ = true;
    return Error::success();
}

void IoEngine::shutdown() {
#if USE_IOURING
    if (io_uring_engine_) {
        io_uring_engine_->shutdown();
    }
#endif  // USE_IOURING
    initialized_ = false;
}

Error IoEngine::submit_read(
    int fd,
    Offset offset,
    char* buffer,
    Size size,
    NodeId node_id,
    IoPriority priority
) {
#if USE_IOURING
    if (use_io_uring_ && io_uring_engine_) {
        return io_uring_engine_->submit_read(fd, offset, buffer, size, node_id, priority);
    }
#endif  // USE_IOURING
    
    // Synchronous fallback
    int result = sync_engine_->read(fd, offset, buffer, size);
    if (result < 0) {
        return Error::io_error("Failed to read from file");
    }
    return Error::success();
}

Error IoEngine::submit_write(
    int fd,
    Offset offset,
    const char* buffer,
    Size size,
    NodeId node_id,
    IoPriority priority
) {
#if USE_IOURING
    if (use_io_uring_ && io_uring_engine_) {
        return io_uring_engine_->submit_write(fd, offset, buffer, size, node_id, priority);
    }
#endif  // USE_IOURING
    
    // Synchronous fallback
    int result = sync_engine_->write(fd, offset, buffer, size);
    if (result < 0) {
        return Error::io_error("Failed to write to file");
    }
    return Error::success();
}

Size IoEngine::submit_batch_read(const std::vector<IoRequest>& requests) {
#if USE_IOURING
    if (use_io_uring_ && io_uring_engine_) {
        return io_uring_engine_->submit_batch(requests);
    }
#endif  // USE_IOURING
    
    // Synchronous fallback
    Size count = 0;
    for (const auto& req : requests) {
        int result = sync_engine_->read(req.fd, req.offset, req.buffer, req.size);
        if (result >= 0) {
            count++;
        }
    }
    return count;
}

IoCompletion IoEngine::wait_completion(int timeout_ms) {
    IoCompletion completion;
#if USE_IOURING
    if (use_io_uring_ && io_uring_engine_) {
        return io_uring_engine_->wait_completion(timeout_ms);
    }
#endif  // USE_IOURING
    return completion;
}

std::vector<IoCompletion> IoEngine::wait_completion_batch(Size max_completions, int timeout_ms) {
#if USE_IOURING
    if (use_io_uring_ && io_uring_engine_) {
        return io_uring_engine_->wait_completion_batch(max_completions, timeout_ms);
    }
#endif  // USE_IOURING
    return {};
}

bool IoEngine::using_io_uring() const {
    return use_io_uring_;
}

bool IoEngine::is_initialized() const {
    return initialized_;
}

#if USE_IOURING
// =============================================================================
// IoUringEngine Implementation
// =============================================================================

IoUringEngine::IoUringEngine(const IoEngineConfig& config)
    : config_(config)
    , initialized_(false)
{
}

IoUringEngine::~IoUringEngine() {
    shutdown();
}

Error IoUringEngine::init() {
    struct io_uring_params params = {};
    
    if (config_.use_polling) {
        params.flags |= IORING_SETUP_IOPOLL;
    }
    if (config_.enable_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
    }
    
    int ret = io_uring_queue_init_params(config_.queue_depth, &ring_, &params);
    if (ret < 0) {
        return Error::io_error("Failed to initialize io_uring: " + std::string(strerror(-ret)));
    }
    
    initialized_ = true;
    return Error::success();
}

void IoUringEngine::shutdown() {
    if (initialized_) {
        io_uring_queue_exit(&ring_);
        initialized_ = false;
    }
}

Error IoUringEngine::submit(const IoRequest& request) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return Error::io_error("Failed to get SQE from io_uring");
    }
    
    if (request.is_read) {
        io_uring_prep_read(sqe, request.fd, request.buffer, request.size, request.offset);
    } else {
        io_uring_prep_write(sqe, request.fd, request.buffer, request.size, request.offset);
    }
    
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(request.node_id)));
    
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        return Error::io_error("Failed to submit to io_uring: " + std::string(strerror(-ret)));
    }
    
    total_submitted_++;
    return Error::success();
}

Size IoUringEngine::submit_batch(const std::vector<IoRequest>& requests) {
    Size count = 0;
    for (const auto& req : requests) {
        Error err = submit(req);
        if (err.ok()) {
            count++;
        }
    }
    return count;
}

Error IoUringEngine::submit_read(
    int fd,
    Offset offset,
    char* buffer,
    Size size,
    NodeId node_id,
    IoPriority priority
) {
    IoRequest req;
    req.fd = fd;
    req.offset = offset;
    req.buffer = buffer;
    req.size = size;
    req.node_id = node_id;
    req.is_read = true;
    return submit(req);
}

Error IoUringEngine::submit_write(
    int fd,
    Offset offset,
    const char* buffer,
    Size size,
    NodeId node_id,
    IoPriority priority
) {
    IoRequest req;
    req.fd = fd;
    req.offset = offset;
    req.buffer = const_cast<char*>(buffer);
    req.size = size;
    req.node_id = node_id;
    req.is_read = false;
    return submit(req);
}

IoCompletion IoUringEngine::wait_completion(int timeout_ms) {
    struct io_uring_cqe* cqe = nullptr;
    
    if (timeout_ms >= 0) {
        struct __kernel_timespec ts = {
            .tv_sec = timeout_ms / 1000,
            .tv_nsec = (timeout_ms % 1000) * 1000000
        };
        io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
    } else {
        io_uring_wait_cqe(&ring_, &cqe);
    }
    
    return process_cqe(cqe);
}

std::vector<IoCompletion> IoUringEngine::wait_completion_batch(Size max_completions, int timeout_ms) {
    std::vector<IoCompletion> completions;
    
    for (Size i = 0; i < max_completions; ++i) {
        IoCompletion comp = wait_completion(timeout_ms);
        if (comp.node_id != INVALID_NODE_ID) {
            completions.push_back(comp);
        } else {
            break;
        }
    }
    
    return completions;
}

std::vector<IoCompletion> IoUringEngine::poll_completions() {
    std::vector<IoCompletion> completions;
    
    unsigned head;
    unsigned count = 0;
    struct io_uring_cqe* cqe;
    
    io_uring_for_each_cqe(&ring_, head, cqe) {
        completions.push_back(process_cqe(cqe));
        count++;
    }
    
    if (count > 0) {
        io_uring_cq_advance(&ring_, count);
    }
    
    return completions;
}

Size IoUringEngine::pending_count() const {
    return io_uring_sq_ready(&ring_);
}

Size IoUringEngine::completed_count() const {
    return io_uring_cq_ready(&ring_);
}

bool IoUringEngine::is_initialized() const {
    return initialized_;
}

Size IoUringEngine::queue_depth() const {
    return config_.queue_depth;
}

IoCompletion IoUringEngine::process_cqe(struct io_uring_cqe* cqe) {
    IoCompletion comp;
    
    if (cqe) {
        comp.node_id = static_cast<NodeId>(reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
        comp.result = cqe->res;
        comp.size = (cqe->res > 0) ? static_cast<Size>(cqe->res) : 0;
        
        io_uring_cqe_seen(&ring_, cqe);
        total_completed_++;
        
        if (cqe->res < 0) {
            total_errors_++;
        }
    }
    
    return comp;
}

int IoUringEngine::register_file(int fd) {
    // TODO: Implement file registration
    return -1;
}

Error IoUringEngine::unregister_file(int index) {
    // TODO: Implement file unregistration
    return Error::success();
}

int IoUringEngine::register_buffer(char* buffer, Size size) {
    // TODO: Implement buffer registration
    return -1;
}

Error IoUringEngine::unregister_buffer(int index) {
    // TODO: Implement buffer unregistration
    return Error::success();
}

void IoUringEngine::prepare_read_sqe(struct io_uring_sqe* sqe, const IoRequest& request, IoPriority priority) {
    io_uring_prep_read(sqe, request.fd, request.buffer, request.size, request.offset);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(request.node_id)));
}

void IoUringEngine::prepare_write_sqe(struct io_uring_sqe* sqe, const IoRequest& request, IoPriority priority) {
    io_uring_prep_write(sqe, request.fd, request.buffer, request.size, request.offset);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(request.node_id)));
}

#endif  // USE_IOURING

}  // namespace agent_mem_io