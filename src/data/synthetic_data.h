/**
 * @file synthetic_data.h
 * @brief Synthetic data generator for testing when SIFT1M is unavailable
 * 
 * Generates random vectors with controlled properties for testing
 * the storage engine and index.
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <random>
#include <cmath>

namespace agent_mem_io {

/**
 * @brief Synthetic data generator
 */
class SyntheticDataGenerator {
public:
    /**
     * @brief Generate random vectors
     * @param num_vectors Number of vectors to generate
     * @param dimension Vector dimension
     * @param seed Random seed for reproducibility
     * @return Vector of random vectors
     */
    static std::vector<Vector> generate_random_vectors(
        Size num_vectors,
        Dimension dimension,
        unsigned int seed = 42
    ) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        
        std::vector<Vector> vectors;
        vectors.reserve(num_vectors);
        
        for (Size i = 0; i < num_vectors; ++i) {
            Vector vec(dimension);
            for (Dimension d = 0; d < dimension; ++d) {
                vec[d] = dist(rng);
            }
            // Normalize
            float norm = 0.0f;
            for (Dimension d = 0; d < dimension; ++d) {
                norm += vec[d] * vec[d];
            }
            norm = std::sqrt(norm);
            if (norm > 0) {
                for (Dimension d = 0; d < dimension; ++d) {
                    vec[d] /= norm;
                }
            }
            vectors.push_back(std::move(vec));
        }
        
        return vectors;
    }
    
    /**
     * @brief Generate clustered vectors (more realistic for ANN search)
     * @param num_vectors Number of vectors to generate
     * @param dimension Vector dimension
     * @param num_clusters Number of clusters
     * @param cluster_std Standard deviation within clusters
     * @param seed Random seed
     * @return Vector of clustered vectors
     */
    static std::vector<Vector> generate_clustered_vectors(
        Size num_vectors,
        Dimension dimension,
        Size num_clusters,
        float cluster_std = 0.1f,
        unsigned int seed = 42
    ) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> normal_dist(0.0f, cluster_std);
        std::uniform_real_distribution<float> uniform_dist(-1.0f, 1.0f);
        
        // Generate cluster centers
        std::vector<Vector> centers;
        centers.reserve(num_clusters);
        for (Size c = 0; c < num_clusters; ++c) {
            Vector center(dimension);
            for (Dimension d = 0; d < dimension; ++d) {
                center[d] = uniform_dist(rng);
            }
            // Normalize center
            float norm = 0.0f;
            for (Dimension d = 0; d < dimension; ++d) {
                norm += center[d] * center[d];
            }
            norm = std::sqrt(norm);
            if (norm > 0) {
                for (Dimension d = 0; d < dimension; ++d) {
                    center[d] /= norm;
                }
            }
            centers.push_back(std::move(center));
        }
        
        // Generate vectors around cluster centers
        std::vector<Vector> vectors;
        vectors.reserve(num_vectors);
        std::uniform_int_distribution<Size> cluster_dist(0, num_clusters - 1);
        
        for (Size i = 0; i < num_vectors; ++i) {
            Size cluster_id = cluster_dist(rng);
            const Vector& center = centers[cluster_id];
            
            Vector vec(dimension);
            for (Dimension d = 0; d < dimension; ++d) {
                vec[d] = center[d] + normal_dist(rng);
            }
            vectors.push_back(std::move(vec));
        }
        
        return vectors;
    }
    
    /**
     * @brief Generate query vectors and ground truth
     * @param base_vectors Base vector set
     * @param num_queries Number of query vectors
     * @param k Number of ground truth neighbors per query
     * @param seed Random seed
     * @return Pair of (query_vectors, ground_truth_neighbors)
     */
    static std::pair<std::vector<Vector>, std::vector<std::vector<NodeId>>> 
    generate_queries_with_groundtruth(
        const std::vector<Vector>& base_vectors,
        Size num_queries,
        Size k,
        unsigned int seed = 42
    ) {
        if (base_vectors.empty()) {
            return {{}, {}};
        }
        
        std::mt19937 rng(seed);
        Dimension dimension = base_vectors[0].size();
        
        // Select random base vectors as queries
        std::uniform_int_distribution<Size> idx_dist(0, base_vectors.size() - 1);
        
        std::vector<Vector> queries;
        queries.reserve(num_queries);
        std::vector<std::vector<NodeId>> groundtruth;
        groundtruth.reserve(num_queries);
        
        for (Size q = 0; q < num_queries; ++q) {
            // Use a base vector as query (with small perturbation)
            Size base_idx = idx_dist(rng);
            Vector query = base_vectors[base_idx];
            
            // Add small noise
            std::normal_distribution<float> noise(0.0f, 0.01f);
            for (Dimension d = 0; d < dimension; ++d) {
                query[d] += noise(rng);
            }
            queries.push_back(std::move(query));
            
            // Find k nearest neighbors (brute force for ground truth)
            std::vector<std::pair<float, NodeId>> distances;
            distances.reserve(base_vectors.size());
            
            for (Size i = 0; i < base_vectors.size(); ++i) {
                float dist = 0.0f;
                for (Dimension d = 0; d < dimension; ++d) {
                    float diff = queries[q][d] - base_vectors[i][d];
                    dist += diff * diff;
                }
                distances.emplace_back(dist, static_cast<NodeId>(i));
            }
            
            // Sort by distance
            std::partial_sort(
                distances.begin(), 
                distances.begin() + std::min(k, distances.size()),
                distances.end()
            );
            
            // Extract top-k neighbors
            std::vector<NodeId> neighbors;
            neighbors.reserve(k);
            for (Size i = 0; i < k && i < distances.size(); ++i) {
                neighbors.push_back(distances[i].second);
            }
            groundtruth.push_back(std::move(neighbors));
        }
        
        return {queries, groundtruth};
    }
    
    /**
     * @brief Calculate L2 distance between two vectors
     */
    static float l2_distance(const Vector& a, const Vector& b) {
        float dist = 0.0f;
        for (Size i = 0; i < a.size(); ++i) {
            float diff = a[i] - b[i];
            dist += diff * diff;
        }
        return std::sqrt(dist);
    }
};

}  // namespace agent_mem_io