/**
 * @file graph_index.h
 * @brief Vamana graph index for vector similarity search
 * 
 * This file implements the Vamana graph index algorithm from DiskANN paper,
 * optimized for SSD-based vector retrieval with memory constraints.
 */

#pragma once

#include "common/types.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace agent_mem_io {

/**
 * @brief Graph index configuration
 */
struct GraphIndexConfig {
    Degree max_degree = DEFAULT_MAX_DEGREE;           // Maximum outgoing edges per node
    Size search_list_size = DEFAULT_SEARCH_LIST_SIZE; // Search list size for graph construction
    Degree init_degree = 64;                          // Initial degree before pruning
    double alpha = 1.2;                               // Pruning parameter (alpha >= 1.0)
    bool use_static_index = false;                    // Whether to use static index (no updates)
    
    GraphIndexConfig() = default;
};

/**
 * @brief Graph navigation data (stored in memory)
 * 
 * This structure holds the graph's adjacency lists and entry point.
 * It's designed to be memory-efficient for large-scale datasets.
 */
class GraphNavData {
public:
    /**
     * @brief Construct graph navigation data
     * @param num_nodes Number of nodes in the graph
     * @param max_degree Maximum degree per node
     */
    GraphNavData(Size num_nodes, Degree max_degree);
    
    /**
     * @brief Get neighbors of a node
     * @param node_id Node identifier
     * @return List of neighbor node IDs
     */
    const std::vector<NodeId>& get_neighbors(NodeId node_id) const;
    
    /**
     * @brief Get mutable neighbors of a node (for construction)
     * @param node_id Node identifier
     * @return Mutable list of neighbor node IDs
     */
    std::vector<NodeId>& get_neighbors_mut(NodeId node_id);
    
    /**
     * @brief Add a neighbor to a node
     * @param node_id Node identifier
     * @param neighbor_id Neighbor to add
     * @return true if added, false if already exists or degree limit reached
     */
    bool add_neighbor(NodeId node_id, NodeId neighbor_id);
    
    /**
     * @brief Remove a neighbor from a node
     * @param node_id Node identifier
     * @param neighbor_id Neighbor to remove
     * @return true if removed, false if not found
     */
    bool remove_neighbor(NodeId node_id, NodeId neighbor_id);
    
    /**
     * @brief Get the degree of a node
     * @param node_id Node identifier
     * @return Number of neighbors
     */
    Degree get_degree(NodeId node_id) const;
    
    /**
     * @brief Get entry point node
     * @return Entry point node ID
     */
    NodeId get_entry_point() const { return entry_point_; }
    
    /**
     * @brief Set entry point node
     * @param node_id New entry point
     */
    void set_entry_point(NodeId node_id) { entry_point_ = node_id; }
    
    /**
     * @brief Get number of nodes
     * @return Number of nodes in the graph
     */
    Size get_num_nodes() const { return num_nodes_; }
    
    /**
     * @brief Get max degree
     * @return Maximum degree per node
     */
    Degree get_max_degree() const { return max_degree_; }
    
    /**
     * @brief Calculate memory usage
     * @return Memory usage in bytes
     */
    Size calculate_memory_usage() const;
    
    /**
     * @brief Clear all graph data
     */
    void clear();

private:
    Size num_nodes_;
    Degree max_degree_;
    NodeId entry_point_;
    
    // Adjacency lists: adjacency_list_[node_id] = [neighbor1, neighbor2, ...]
    std::vector<std::vector<NodeId>> adjacency_list_;
    
    // Mutex for thread-safe access
    mutable std::shared_mutex mutex_;
};

/**
 * @brief Vamana graph index builder
 * 
 * Implements the Vamana algorithm for building a disk-based graph index.
 * The algorithm creates a graph with good search quality while minimizing
 * random I/O during search.
 */
class VamanaBuilder {
public:
    /**
     * @brief Construct Vamana builder
     * @param config Graph index configuration
     */
    explicit VamanaBuilder(const GraphIndexConfig& config);
    
    /**
     * @brief Build graph index from dataset
     * @param vectors Vector dataset (stored in memory for building)
     * @param nav_data Graph navigation data to populate
     * @return Error status
     */
    Error build(const std::vector<Vector>& vectors, GraphNavData& nav_data);
    
    /**
     * @brief Build graph index incrementally (for dynamic updates)
     * @param new_vector New vector to add
     * @param new_node_id Node ID for the new vector
     * @param nav_data Existing graph navigation data
     * @param distance_func Distance function
     * @return Error status
     */
    Error add_node_incremental(
        const Vector& new_vector,
        NodeId new_node_id,
        GraphNavData& nav_data,
        std::function<Distance(const Vector&, const Vector&)> distance_func
    );
    
    /**
     * @brief Prune neighbors of a node using robust prune algorithm
     * @param node_id Node to prune
     * @param candidates Candidate neighbors with distances
     * @param vectors Vector dataset
     * @param nav_data Graph navigation data
     * @return Pruned neighbor list
     */
    std::vector<NodeId> robust_prune(
        NodeId node_id,
        const std::vector<Neighbor>& candidates,
        const std::vector<Vector>& vectors,
        GraphNavData& nav_data
    );

private:
    /**
     * @brief Search for nearest neighbors (used during construction)
     * @param query Query vector
     * @param vectors Vector dataset
     * @param nav_data Graph navigation data
     * @param k Number of neighbors to find
     * @return List of nearest neighbors with distances
     */
    std::vector<Neighbor> search_for_construction(
        const Vector& query,
        const std::vector<Vector>& vectors,
        const GraphNavData& nav_data,
        Size k
    );
    
    /**
     * @brief Initialize random graph
     * @param vectors Vector dataset
     * @param nav_data Graph navigation data to populate
     */
    void init_random_graph(const std::vector<Vector>& vectors, GraphNavData& nav_data);
    
    GraphIndexConfig config_;
};

/**
 * @brief Graph index class (combines navigation data and builder)
 */
class GraphIndex {
public:
    /**
     * @brief Construct graph index
     * @param config Graph index configuration
     */
    explicit GraphIndex(const GraphIndexConfig& config = GraphIndexConfig());
    
    /**
     * @brief Build index from vectors
     * @param vectors Vector dataset
     * @return Error status
     */
    Error build(const std::vector<Vector>& vectors);
    
    /**
     * @brief Add a new vector to the index
     * @param vector New vector
     * @param node_id Assigned node ID
     * @return Error status
     */
    Error add_vector(const Vector& vector, NodeId node_id);
    
    /**
     * @brief Get neighbors of a node
     * @param node_id Node identifier
     * @return List of neighbor node IDs
     */
    const std::vector<NodeId>& get_neighbors(NodeId node_id) const;
    
    /**
     * @brief Get entry point
     * @return Entry point node ID
     */
    NodeId get_entry_point() const;
    
    /**
     * @brief Get number of nodes
     * @return Number of nodes in the index
     */
    Size get_num_nodes() const;
    
    /**
     * @brief Get memory usage
     * @return Memory usage in bytes
     */
    Size get_memory_usage() const;
    
    /**
     * @brief Save graph to file
     * @param filepath File path
     * @return Error status
     */
    Error save(const std::string& filepath) const;
    
    /**
     * @brief Load graph from file
     * @param filepath File path
     * @return Error status
     */
    Error load(const std::string& filepath);
    
    /**
     * @brief Get navigation data (for search)
     * @return Pointer to graph navigation data
     */
    const GraphNavData* get_nav_data() const { return nav_data_.get(); }
    
    /**
     * @brief Check if index is built
     * @return true if index is built
     */
    bool is_built() const { return is_built_; }

private:
    GraphIndexConfig config_;
    std::unique_ptr<GraphNavData> nav_data_;
    std::unique_ptr<VamanaBuilder> builder_;
    bool is_built_;
    
    mutable std::mutex mutex_;
};

}  // namespace agent_mem_io