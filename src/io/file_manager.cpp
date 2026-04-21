/**
 * @file file_manager.cpp
 * @brief File management utilities implementation
 *
 * Provides file lifecycle management for the storage engine:
 *   - Data directory creation and validation
 *   - Index file management (vector, graph, metadata)
 *   - Temporary file handling for compaction
 *   - File size tracking and quota enforcement
 */

#include "io/io_uring_engine.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace agent_mem_io {

// =============================================================================
// Directory Management
// =============================================================================

/**
 * @brief Create a data directory with proper permissions
 * @param path Directory path
 * @return true if directory exists (created or already existed)
 */
bool ensure_data_directory(const std::string& path) {
    if (std::filesystem::exists(path)) {
        return std::filesystem::is_directory(path);
    }

    try {
        std::filesystem::create_directories(path);
        std::filesystem::permissions(path,
            std::filesystem::perms::owner_all |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec);
        return true;
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[FileManager] Failed to create directory " << path
                  << ": " << e.what() << "\n";
        return false;
    }
}

/**
 * @brief Validate a data directory: check that required files exist
 * @param path Directory path
 * @return true if valid
 */
bool validate_data_directory(const std::string& path) {
    if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
        std::cerr << "[FileManager] Directory does not exist: " << path << "\n";
        return false;
    }

    // Check for essential files
    std::string index_file = path + "/disk_index.bin";
    if (!std::filesystem::exists(index_file)) {
        std::cerr << "[FileManager] Index file missing: " << index_file << "\n";
        // Not a fatal error — may be first run
    }

    return true;
}

// =============================================================================
// File Size Tracking
// =============================================================================

/**
 * @brief Get file size in bytes
 * @param filepath Path to file
 * @return File size in bytes, or 0 if file doesn't exist
 */
Size get_file_size(const std::string& filepath) {
    if (!std::filesystem::exists(filepath)) {
        return 0;
    }

    try {
        return static_cast<Size>(std::filesystem::file_size(filepath));
    } catch (const std::filesystem::filesystem_error&) {
        struct stat st;
        if (stat(filepath.c_str(), &st) == 0) {
            return static_cast<Size>(st.st_size);
        }
        return 0;
    }
}

/**
 * @brief Get total data directory size (sum of all files)
 * @param path Directory path
 * @return Total size in bytes
 */
Size get_directory_size(const std::string& path) {
    Size total = 0;

    if (!std::filesystem::exists(path)) {
        return 0;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                total += static_cast<Size>(entry.file_size());
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[FileManager] Failed to iterate directory " << path
                  << ": " << e.what() << "\n";
    }

    return total;
}

// =============================================================================
// Temporary File Management
// =============================================================================

/**
 * @brief Create a temporary file for compaction output
 * @param data_dir Data directory for temporary file
 * @param suffix File suffix (e.g., ".sst.tmp")
 * @return Temporary file path
 */
std::string create_temp_file(const std::string& data_dir, const std::string& suffix) {
    // Generate unique temporary filename using PID and timestamp
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::string filename = data_dir + "/tmp_" + std::to_string(getpid())
                          + "_" + std::to_string(now) + suffix;
    return filename;
}

/**
 * @brief Rename a temporary file to its final name (atomic on same filesystem)
 * @param temp_path Temporary file path
 * @param final_path Final file path
 * @return true if rename succeeded
 */
bool commit_temp_file(const std::string& temp_path, const std::string& final_path) {
    try {
        std::filesystem::rename(temp_path, final_path);
        return true;
    } catch (const std::filesystem::filesystem_error& e) {
        // Fallback to C rename
        if (::rename(temp_path.c_str(), final_path.c_str()) == 0) {
            return true;
        }
        std::cerr << "[FileManager] Failed to rename " << temp_path
                  << " to " << final_path << ": " << e.what() << "\n";
        return false;
    }
}

/**
 * @brief Delete a temporary file (cleanup after failed compaction)
 * @param temp_path Temporary file path
 * @return true if file was deleted or didn't exist
 */
bool delete_temp_file(const std::string& temp_path) {
    if (!std::filesystem::exists(temp_path)) {
        return true;
    }

    try {
        return std::filesystem::remove(temp_path);
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[FileManager] Failed to delete temp file " << temp_path
                  << ": " << e.what() << "\n";
        return false;
    }
}

// =============================================================================
// Index File Management
// =============================================================================

/**
 * @brief List all SSTable files in a data directory
 * @param data_dir Data directory
 * @return Vector of SSTable filenames (sorted by sequence number)
 */
std::vector<std::string> list_sstable_files(const std::string& data_dir) {
    std::vector<std::string> files;

    if (!std::filesystem::exists(data_dir)) {
        return files;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find(".sst") != std::string::npos &&
                    filename.find(".tmp") == std::string::npos) {
                    files.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Ignore errors — return whatever we found
    }

    // Sort by filename (which encodes sequence number)
    std::sort(files.begin(), files.end());

    return files;
}

/**
 * @brief Clean up stale temporary files in data directory
 * @param data_dir Data directory
 * @return Number of files cleaned up
 */
Size cleanup_stale_temp_files(const std::string& data_dir) {
    Size count = 0;

    if (!std::filesystem::exists(data_dir)) {
        return 0;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find(".tmp") != std::string::npos) {
                    std::filesystem::remove(entry.path());
                    count++;
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Ignore errors
    }

    if (count > 0) {
        std::cout << "[FileManager] Cleaned up " << count << " stale temp files\n";
    }

    return count;
}

}  // namespace agent_mem_io