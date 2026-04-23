/**
 * @file sift_loader.h
 * @brief SIFT1M dataset loader for .fvecs and .ivecs file formats
 * 
 * File formats:
 * - .fvecs: float vectors, each vector prefixed with dimension (int32)
 * - .ivecs: int32 vectors, each vector prefixed with dimension (int32)
 * 
 * Binary format:
 * [dimension (4 bytes)] [vector data (dimension * 4 bytes)] ...
 */

#pragma once

#include "common/types.h"
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace agent_mem_io {

/**
 * @brief SIFT dataset loader
 */
class SiftLoader {
public:
    /**
     * @brief Load float vectors from .fvecs file
     * @param filepath Path to .fvecs file
     * @return Vector of vectors (num_vectors x dimension)
     */
    /**
     * @brief Load vectors from .fvecs file with optional subset limit
     * @param filepath Path to .fvecs file
     * @param max_vectors Maximum number of vectors to load (0 = all)
     * @return Vector of float vectors
     */
    static std::vector<Vector> load_fvecs(const std::string& filepath, Size max_vectors = 0) {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filepath);
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        Size file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::vector<Vector> vectors;
        Dimension dimension = 0;
        Size num_vectors = 0;
        
        // Read first vector to get dimension
        int32_t dim;
        file.read(reinterpret_cast<char*>(&dim), sizeof(int32_t));
        dimension = static_cast<Dimension>(dim);
        
        if (dimension <= 0 || dimension > 10000) {
            throw std::runtime_error("Invalid dimension: " + std::to_string(dimension));
        }
        
        // Calculate number of vectors
        Size vector_bytes = sizeof(int32_t) + dimension * sizeof(float);
        num_vectors = file_size / vector_bytes;
        
        // Apply subset limit if specified
        if (max_vectors > 0 && max_vectors < num_vectors) {
            num_vectors = max_vectors;
        }
        
        // Reset to beginning
        file.seekg(0, std::ios::beg);
        
        // Reserve space
        vectors.reserve(num_vectors);
        
        // Read vectors (up to num_vectors)
        for (Size i = 0; i < num_vectors; ++i) {
            int32_t d;
            file.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
            
            if (d != dimension) {
                throw std::runtime_error("Dimension mismatch at vector " + 
                    std::to_string(i) + ": expected " + std::to_string(dimension) +
                    ", got " + std::to_string(d));
            }
            
            Vector vec(dimension);
            file.read(reinterpret_cast<char*>(vec.data()), dimension * sizeof(float));
            vectors.push_back(std::move(vec));
        }
        
        return vectors;
    }
    
    /**
     * @brief Load int32 vectors from .ivecs file
     * @param filepath Path to .ivecs file
     * @return Vector of int32 vectors
     */
    static std::vector<std::vector<int32_t>> load_ivecs(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filepath);
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        Size file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::vector<std::vector<int32_t>> vectors;
        
        // Read all vectors
        while (file.tellg() < static_cast<std::streamoff>(file_size)) {
            int32_t dim;
            file.read(reinterpret_cast<char*>(&dim), sizeof(int32_t));
            
            if (dim <= 0 || dim > 10000) {
                throw std::runtime_error("Invalid dimension: " + std::to_string(dim));
            }
            
            std::vector<int32_t> vec(dim);
            file.read(reinterpret_cast<char*>(vec.data()), dim * sizeof(int32_t));
            vectors.push_back(std::move(vec));
        }
        
        return vectors;
    }
    
    /**
     * @brief Load ground truth neighbors
     * @param filepath Path to .ivecs file containing ground truth
     * @return Vector of neighbor ID vectors
     */
    static std::vector<std::vector<NodeId>> load_groundtruth(const std::string& filepath) {
        auto ivecs = load_ivecs(filepath);
        std::vector<std::vector<NodeId>> groundtruth;
        groundtruth.reserve(ivecs.size());
        
        for (const auto& vec : ivecs) {
            std::vector<NodeId> neighbors;
            neighbors.reserve(vec.size());
            for (int32_t id : vec) {
                neighbors.push_back(static_cast<NodeId>(id));
            }
            groundtruth.push_back(std::move(neighbors));
        }
        
        return groundtruth;
    }
    
    /**
     * @brief Get dataset statistics
     * @param filepath Path to .fvecs file
     * @return Pair of (num_vectors, dimension)
     */
    static std::pair<Size, Dimension> get_fvecs_stats(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filepath);
        }
        
        // Get file size
        file.seekg(0, std::ios::end);
        Size file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        // Read dimension
        int32_t dim;
        file.read(reinterpret_cast<char*>(&dim), sizeof(int32_t));
        Dimension dimension = static_cast<Dimension>(dim);
        
        // Calculate number of vectors
        Size vector_bytes = sizeof(int32_t) + dimension * sizeof(float);
        Size num_vectors = file_size / vector_bytes;
        
        return {num_vectors, dimension};
    }
    
    /**
     * @brief Calculate Recall@K
     * @param results Search results (query_id -> result_node_ids)
     * @param groundtruth Ground truth (query_id -> true_neighbor_ids)
     * @param k Number of neighbors to consider
     * @return Recall value [0, 1]
     */
    static float calculate_recall(
        const std::vector<std::vector<NodeId>>& results,
        const std::vector<std::vector<NodeId>>& groundtruth,
        Size k
    ) {
        if (results.size() != groundtruth.size()) {
            throw std::runtime_error("Result and groundtruth size mismatch");
        }
        
        Size total_queries = results.size();
        Size total_hits = 0;
        Size total_relevant = 0;
        
        for (Size q = 0; q < total_queries; ++q) {
            const auto& res = results[q];
            const auto& gt = groundtruth[q];
            
            // Create set of ground truth neighbors (top-k)
            std::unordered_set<NodeId> gt_set;
            Size gt_k = std::min(k, static_cast<Size>(gt.size()));
            for (Size i = 0; i < gt_k; ++i) {
                gt_set.insert(gt[i]);
            }
            
            // Count hits
            Size res_k = std::min(k, static_cast<Size>(res.size()));
            for (Size i = 0; i < res_k; ++i) {
                if (gt_set.count(res[i]) > 0) {
                    total_hits++;
                }
            }
            
            total_relevant += gt_k;
        }
        
        return static_cast<float>(total_hits) / static_cast<float>(total_relevant);
    }
};

}  // namespace agent_mem_io