/**
 * @file distance.cpp
 * @brief Distance calculation functions for vector similarity
 */

#include "common/types.h"
#include <cmath>
#include <algorithm>

namespace agent_mem_io {

Distance l2_distance(const Vector& v1, const Vector& v2) {
    if (v1.size() != v2.size()) {
        throw StorageEngineException(
            ErrorCode::INVALID_ARGUMENT,
            "Vector dimensions do not match for L2 distance calculation"
        );
    }
    
    Distance sum = 0.0f;
    for (Size i = 0; i < v1.size(); ++i) {
        Distance diff = v1[i] - v2[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

Distance inner_product_distance(const Vector& v1, const Vector& v2) {
    if (v1.size() != v2.size()) {
        throw StorageEngineException(
            ErrorCode::INVALID_ARGUMENT,
            "Vector dimensions do not match for inner product calculation"
        );
    }
    
    Distance sum = 0.0f;
    for (Size i = 0; i < v1.size(); ++i) {
        sum += v1[i] * v2[i];
    }
    // Return negative inner product as distance (larger inner product = smaller distance)
    return -sum;
}

Distance cosine_distance(const Vector& v1, const Vector& v2) {
    if (v1.size() != v2.size()) {
        throw StorageEngineException(
            ErrorCode::INVALID_ARGUMENT,
            "Vector dimensions do not match for cosine distance calculation"
        );
    }
    
    Distance dot_product = 0.0f;
    Distance norm1 = 0.0f;
    Distance norm2 = 0.0f;
    
    for (Size i = 0; i < v1.size(); ++i) {
        dot_product += v1[i] * v2[i];
        norm1 += v1[i] * v1[i];
        norm2 += v2[i] * v2[i];
    }
    
    norm1 = std::sqrt(norm1);
    norm2 = std::sqrt(norm2);
    
    if (norm1 == 0.0f || norm2 == 0.0f) {
        return 1.0f;  // Maximum distance for zero vectors
    }
    
    Distance cosine_similarity = dot_product / (norm1 * norm2);
    // Cosine distance = 1 - cosine_similarity
    return 1.0f - cosine_similarity;
}

}  // namespace agent_mem_io