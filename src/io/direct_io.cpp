/**
 * @file direct_io.cpp
 * @brief Direct I/O utilities implementation
 *
 * Provides helper functions for O_DIRECT I/O operations:
 *   - Aligned buffer allocation/deallocation
 *   - O_DIRECT file open with fallback
 *   - Aligned pread/pwrite wrappers
 *   - Batch aligned I/O operations
 */

#include "io/io_uring_engine.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <vector>

namespace agent_mem_io {

// =============================================================================
// Aligned Buffer Management
// =============================================================================

/**
 * @brief Allocate a buffer aligned to O_DIRECT requirements (4KB boundary)
 * @param size Buffer size (will be rounded up to 4KB alignment)
 * @return Aligned buffer, or nullptr on failure
 */
char* alloc_direct_io_buffer(Size size) {
    // Round up to 4KB alignment
    Size aligned_size = ((size + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

    void* ptr = nullptr;
    int ret = posix_memalign(&ptr, ALIGNMENT, aligned_size);
    if (ret != 0) {
        std::cerr << "[DirectIO] posix_memalign failed for " << aligned_size
                  << " bytes (error " << ret << ")\n";
        return nullptr;
    }

    std::memset(ptr, 0, aligned_size);
    return static_cast<char*>(ptr);
}

/**
 * @brief Free a Direct I/O aligned buffer
 * @param buffer Buffer allocated by alloc_direct_io_buffer()
 */
void free_direct_io_buffer(char* buffer) {
    if (buffer) {
        free(buffer);
    }
}

// =============================================================================
// O_DIRECT File Operations
// =============================================================================

/**
 * @brief Open a file with O_DIRECT, falling back to normal open if O_DIRECT fails
 * @param filepath File path
 * @param read_only true for read-only, false for read-write
 * @param create Create file if it doesn't exist
 * @return File descriptor, or -1 on failure
 */
int open_direct_io_file(const std::string& filepath, bool read_only, bool create) {
    int flags = O_DIRECT;
    if (read_only) {
        flags |= O_RDONLY;
    } else {
        flags |= O_RDWR;
        if (create) {
            flags |= O_CREAT;
        }
    }

    int fd = ::open(filepath.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        // O_DIRECT may fail on tmpfs or certain filesystems
        std::cerr << "[DirectIO] O_DIRECT open failed for " << filepath
                  << ": " << strerror(errno) << ", trying without O_DIRECT\n";

        flags &= ~O_DIRECT;
        fd = ::open(filepath.c_str(), flags, S_IRUSR | S_IWUSR);
    }

    if (fd < 0) {
        std::cerr << "[DirectIO] Failed to open " << filepath
                  << ": " << strerror(errno) << "\n";
        return -1;
    }

    return fd;
}

/**
 * @brief Close a Direct I/O file descriptor
 * @param fd File descriptor to close
 */
void close_direct_io_file(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

// =============================================================================
// Aligned I/O Wrappers
// =============================================================================

/**
 * @brief Aligned pread: read from file at offset into aligned buffer
 * @param fd File descriptor (opened with O_DIRECT)
 * @param buffer Aligned buffer (must be ALIGNMENT-aligned, size >= aligned_count)
 * @param count Number of bytes to read (must be ALIGNMENT-aligned)
 * @param offset File offset (must be ALIGNMENT-aligned)
 * @return Number of bytes read, or -1 on error
 */
ssize_t aligned_pread(int fd, char* buffer, Size count, Offset offset) {
    // Verify alignment requirements
    Size aligned_count = ((count + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

    ssize_t bytes_read = ::pread(fd, buffer, aligned_count, offset);
    if (bytes_read < 0) {
        std::cerr << "[DirectIO] pread failed at offset " << offset
                  << ": " << strerror(errno) << "\n";
        return -1;
    }

    return bytes_read;
}

/**
 * @brief Aligned pwrite: write to file at offset from aligned buffer
 * @param fd File descriptor (opened with O_DIRECT)
 * @param buffer Aligned buffer (must be ALIGNMENT-aligned)
 * @param count Number of bytes to write (must be ALIGNMENT-aligned)
 * @param offset File offset (must be ALIGNMENT-aligned)
 * @return Number of bytes written, or -1 on error
 */
ssize_t aligned_pwrite(int fd, const char* buffer, Size count, Offset offset) {
    Size aligned_count = ((count + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

    ssize_t bytes_written = ::pwrite(fd, buffer, aligned_count, offset);
    if (bytes_written < 0) {
        std::cerr << "[DirectIO] pwrite failed at offset " << offset
                  << ": " << strerror(errno) << "\n";
        return -1;
    }

    return bytes_written;
}

/**
 * @brief Batch aligned pread: read multiple 4KB records in one syscall group
 *
 * For O_DIRECT, each read must be 4KB-aligned. This function reads
 * multiple fixed-size records, issuing individual preads for each.
 * (preadv with non-contiguous offsets requires separate calls anyway.)
 *
 * @param fd File descriptor
 * @param offsets Array of file offsets (each must be 4KB-aligned)
 * @param buffers Array of aligned buffers (each must be 4KB-aligned)
 * @param record_size Size of each record (typically 4KB)
 * @param count Number of records to read
 * @return Number of successfully read records
 */
Size batch_aligned_pread(int fd, const Offset* offsets, char** buffers,
                          Size record_size, Size count) {
    Size success = 0;

    for (Size i = 0; i < count; ++i) {
        ssize_t bytes = aligned_pread(fd, buffers[i], record_size, offsets[i]);
        if (bytes >= 0) {
            success++;
        }
    }

    return success;
}

// =============================================================================
// I/O Advisory
// =============================================================================

/**
 * @brief Advise the kernel about expected read pattern
 * @param fd File descriptor
 * @param offset Start offset
 * @param len Length of the advised range
 * @param advice POSIX fadvise advice (POSIX_FADV_RANDOM, POSIX_FADV_SEQUENTIAL, etc.)
 * @return 0 on success, -1 on error
 */
int advise_read_pattern(int fd, Offset offset, Size len, int advice) {
    return posix_fadvise(fd, offset, len, advice);
}

/**
 * @brief Advise random access pattern for vector index files
 *
 * Vector index reads are random (graph traversal), not sequential.
 * Advising the kernel helps it avoid wasteful readahead.
 *
 * @param fd File descriptor
 * @param file_size Total file size
 * @return 0 on success, -1 on error
 */
int advise_random_access(int fd, Size file_size) {
    return advise_read_pattern(fd, 0, file_size, POSIX_FADV_RANDOM);
}

}  // namespace agent_mem_io