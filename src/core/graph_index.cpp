/**
 * @file graph_index.cpp
 * @brief Vamana graph index implementation
 */

#include "core/graph_index.h"
#include <random>
#include <algorithm>
#include <queue>
#include <fstream>

namespace agent_mem_io {

// =============================================================================
// GraphNavData Implementation
// =============================================================================

GraphNavData::GraphNavData(Size num_nodes, Degree max_degree)
    : num_nodes_(num_nodes)
    , max_degree_(max_degree)
    , entry_point_(INVALID_NODE_ID)
    , adjacency_list_(num_nodes)
{
    // Reserve space for each adjacency list
    for (auto& neighbors : adjacency_list_) {
        neighbors.reserve(max_degree);
    }
}

NodeId GraphNavData::add_node() {
    NodeId new_id = num_nodes_;
    num_nodes_++;
    adjacency_list_.emplace_back();
    adjacency_list_.back().reserve(max_degree_);
    return new_id;
}

const std::vector<NodeId>& GraphNavData::get_neighbors(NodeId node_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (node_id >= num_nodes_) {
        throw StorageEngineException(
            ErrorCode::NODE_NOT_FOUND,
            "Node ID out of range: " + std::to_string(node_id)
        );
    }
    return adjacency_list_[node_id];
}

std::vector<NodeId>& GraphNavData::get_neighbors_mut(NodeId node_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (node_id >= num_nodes_) {
        throw StorageEngineException(
            ErrorCode::NODE_NOT_FOUND,
            "Node ID out of range: " + std::to_string(node_id)
        );
    }
    return adjacency_list_[node_id];
}

bool GraphNavData::add_neighbor(NodeId node_id, NodeId neighbor_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (node_id >= num_nodes_ || neighbor_id >= num_nodes_) {
        return false;
    }
    
    auto& neighbors = adjacency_list_[node_id];
    
    // Check if already exists
    if (std::find(neighbors.begin(), neighbors.end(), neighbor_id) != neighbors.end()) {
        return false;
    }
    
    // Check degree limit
    if (neighbors.size() >= max_degree_) {
        return false;
    }
    
    neighbors.push_back(neighbor_id);
    return true;
}

bool GraphNavData::remove_neighbor(NodeId node_id, NodeId neighbor_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (node_id >= num_nodes_) {
        return false;
    }
    
    auto& neighbors = adjacency_list_[node_id];
    auto it = std::find(neighbors.begin(), neighbors.end(), neighbor_id);
    if (it == neighbors.end()) {
        return false;
    }
    
    neighbors.erase(it);
    return true;
}

Degree GraphNavData::get_degree(NodeId node_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (node_id >= num_nodes_) {
        return 0;
    }
    return static_cast<Degree>(adjacency_list_[node_id].size());
}

Size GraphNavData::calculate_memory_usage() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    Size total = sizeof(num_nodes_) + sizeof(max_degree_) + sizeof(entry_point_);
    total += adjacency_list_.size() * sizeof(std::vector<NodeId>);
    
    for (const auto& neighbors : adjacency_list_) {
        total += neighbors.size() * sizeof(NodeId);
    }
    
    return total;
}

void GraphNavData::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& neighbors : adjacency_list_) {
        neighbors.clear();
    }
    entry_point_ = INVALID_NODE_ID;
}

// =============================================================================
// VamanaBuilder Implementation
// =============================================================================

VamanaBuilder::VamanaBuilder(const GraphIndexConfig& config)
    : config_(config)
{
}

Error VamanaBuilder::build(const std::vector<Vector>& vectors, GraphNavData& nav_data) {
    if (vectors.empty()) {
        return Error::invalid_argument("Empty vector dataset");
    }
    
    Size num_nodes = vectors.size();
    
    // Initialize random graph
    init_random_graph(vectors, nav_data);
    
    // Find medoid as entry point
    Dimension actual_dim = vectors[0].size();
    Vector centroid(actual_dim, 0.0f);
    for (const auto& vec : vectors) {
        for (Size i = 0; i < vec.size(); ++i) {
            centroid[i] += vec[i];
        }
    }
    for (Size i = 0; i < centroid.size(); ++i) {
        centroid[i] /= static_cast<float>(num_nodes);
    }
    
    // Find closest to centroid
    Distance min_dist = std::numeric_limits<Distance>::max();
    NodeId medoid = 0;
    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        Distance dist = l2_distance(centroid, vectors[i]);
        if (dist < min_dist) {
            min_dist = dist;
            medoid = i;
        }
    }
    nav_data.set_entry_point(medoid);
    
    // Iterative optimization
    // For each node, search for neighbors and prune
    std::vector<NodeId> order(num_nodes);
    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        order[i] = i;
    }
    
    // Random permutation for better convergence
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(order.begin(), order.end(), g);
    
    for (NodeId node_id : order) {
        // Search for candidate neighbors
        auto candidates = search_for_construction(
            vectors[node_id],
            vectors,
            nav_data,
            config_.search_list_size
        );
        
        // Prune neighbors
        auto pruned = robust_prune(node_id, candidates, vectors, nav_data);
        
        // Update adjacency list
        auto& neighbors = nav_data.get_neighbors_mut(node_id);
        neighbors.clear();
        for (NodeId neighbor : pruned) {
            if (neighbors.size() < config_.max_degree) {
                neighbors.push_back(neighbor);
            }
        }
        
        // Add reverse edges
        for (NodeId neighbor : neighbors) {
            nav_data.add_neighbor(neighbor, node_id);
        }
    }
    
    return Error::success();
}

Error VamanaBuilder::add_node_incremental(
    const std::vector<Vector>& vectors,
    const Vector& new_vector,
    NodeId new_node_id,
    GraphNavData& nav_data
) {
    // DiskANN-style incremental node insertion (StitchedVamana algorithm):
    //   1. Greedy search from entry point to find nearest neighbor candidates
    //   2. Robust prune to select optimal neighbor set for the new node
    //   3. Add reverse edges (new_node appears as neighbor of its neighbors)
    //   4. Re-prune any neighbor that exceeds max_degree due to reverse edges
    //
    // This is the core of Agent dynamic memory: new experiences (vectors)
    // can be inserted into the graph index without rebuilding it entirely.

    if (nav_data.get_num_nodes() == 0) {
        nav_data.set_entry_point(new_node_id);
        return Error::success();
    }

    // Step 1: Search for nearest neighbor candidates using beam search
    // from the current entry point. This is identical to the search
    // used during graph construction (search_for_construction).
    auto candidates = search_for_construction(
        new_vector, vectors, nav_data, config_.search_list_size
    );

    if (candidates.empty()) {
        // Fallback: connect to entry point if search found nothing
        Distance ep_dist = l2_distance(new_vector, vectors[nav_data.get_entry_point()]);
        candidates.push_back({nav_data.get_entry_point(), ep_dist});
    }

    // Step 2: Robust prune to select optimal neighbors for new_node
    auto pruned = robust_prune(new_node_id, candidates, vectors, nav_data);

    // Set the new node's neighbors
    auto& new_neighbors = nav_data.get_neighbors_mut(new_node_id);
    new_neighbors.clear();
    for (NodeId neighbor : pruned) {
        if (new_neighbors.size() < config_.max_degree) {
            new_neighbors.push_back(neighbor);
        }
    }

    // Step 3: Add reverse edges and re-prune if needed
    for (NodeId neighbor_id : new_neighbors) {
        nav_data.add_neighbor(neighbor_id, new_node_id);

        // Step 4: If neighbor now exceeds max_degree, re-prune it
        // using the same robust_prune algorithm to maintain graph quality
        if (nav_data.get_degree(neighbor_id) > config_.max_degree) {
            // Build candidate set for re-pruning: existing neighbors + new node
            std::vector<Neighbor> re_candidates;
            const std::vector<NodeId>& existing_neighbors = nav_data.get_neighbors(neighbor_id);
            for (NodeId nid : existing_neighbors) {
                Distance dist = l2_distance(vectors[neighbor_id], vectors[nid]);
                re_candidates.push_back({nid, dist});
            }

            auto re_pruned = robust_prune(neighbor_id, re_candidates, vectors, nav_data);

            auto& neighbor_list = nav_data.get_neighbors_mut(neighbor_id);
            neighbor_list.clear();
            for (NodeId pn : re_pruned) {
                neighbor_list.push_back(pn);
            }
        }
    }

    // Optionally update entry point if new node is closer to centroid
    // (helps maintain good search starting point for future queries)
    // This is a lightweight heuristic: just check if the new node
    // has high in-degree (many reverse edges → likely a hub node)

    return Error::success();
}

std::vector<Neighbor> VamanaBuilder::search(
    const Vector& query,
    const std::vector<Vector>& vectors,
    const GraphNavData& nav_data,
    Size ef_search
) {
    return search_for_construction(query, vectors, nav_data, ef_search);
}

std::vector<NodeId> VamanaBuilder::robust_prune(
    NodeId node_id,
    const std::vector<Neighbor>& candidates,
    const std::vector<Vector>& vectors,
    GraphNavData& nav_data
) {
    if (candidates.empty()) {
        return {};
    }
    
    // Sort candidates by distance
    std::vector<Neighbor> sorted_candidates = candidates;
    std::sort(sorted_candidates.begin(), sorted_candidates.end(),
              [](const Neighbor& a, const Neighbor& b) {
                  return a.second < b.second;
              });
    
    std::vector<NodeId> result;
    result.reserve(config_.max_degree);
    
    const Vector& node_vec = vectors[node_id];
    
    for (const auto& [candidate_id, candidate_dist] : sorted_candidates) {
        if (result.size() >= config_.max_degree) {
            break;
        }
        
        if (candidate_id == node_id) {
            continue;
        }
        
        // Check if candidate is closer to node than to any existing neighbor
        bool should_add = true;
        for (NodeId existing_neighbor : result) {
            Distance dist_to_existing = l2_distance(
                vectors[candidate_id],
                vectors[existing_neighbor]
            );
            
            // Robust prune condition: alpha * dist(node, candidate) > dist(candidate, existing)
            if (dist_to_existing < config_.alpha * candidate_dist) {
                should_add = false;
                break;
            }
        }
        
        if (should_add) {
            result.push_back(candidate_id);
        }
    }
    
    return result;
}

std::vector<Neighbor> VamanaBuilder::search_for_construction(
    const Vector& query,
    const std::vector<Vector>& vectors,
    const GraphNavData& nav_data,
    Size k
) {
    std::priority_queue<Neighbor, std::vector<Neighbor>, std::greater<Neighbor>> best;
    std::priority_queue<Neighbor, std::vector<Neighbor>, std::less<Neighbor>> candidates;
    std::unordered_set<NodeId> visited;
    
    // Start from entry point
    NodeId entry = nav_data.get_entry_point();
    Distance entry_dist = l2_distance(query, vectors[entry]);
    
    best.emplace(entry, entry_dist);
    candidates.emplace(entry, entry_dist);
    visited.insert(entry);
    
    while (!candidates.empty()) {
        auto [current_id, current_dist] = candidates.top();
        candidates.pop();
        
        // Stop if current is worse than the k-th best
        if (best.size() >= k && current_dist > best.top().second) {
            break;
        }
        
        // Explore neighbors
        for (NodeId neighbor : nav_data.get_neighbors(current_id)) {
            if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                
                Distance neighbor_dist = l2_distance(query, vectors[neighbor]);
                
                candidates.emplace(neighbor, neighbor_dist);
                best.emplace(neighbor, neighbor_dist);
                
                // Keep only top k in best
                if (best.size() > k) {
                    best.pop();
                }
            }
        }
    }
    
    // Extract results
    std::vector<Neighbor> result;
    while (!best.empty()) {
        result.push_back(best.top());
        best.pop();
    }
    std::reverse(result.begin(), result.end());
    
    return result;
}

void VamanaBuilder::init_random_graph(const std::vector<Vector>& vectors, GraphNavData& nav_data) {
    Size num_nodes = vectors.size();
    std::random_device rd;
    std::mt19937 g(rd());
    
    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        // Randomly select R neighbors
        std::vector<NodeId> all_nodes(num_nodes);
        for (NodeId j = 0; j < static_cast<NodeId>(num_nodes); ++j) {
            all_nodes[j] = j;
        }
        std::shuffle(all_nodes.begin(), all_nodes.end(), g);
        
        auto& neighbors = nav_data.get_neighbors_mut(i);
        neighbors.clear();
        
        for (Size j = 0; j < std::min(static_cast<Size>(config_.init_degree), num_nodes - 1); ++j) {
            if (all_nodes[j] != i) {
                neighbors.push_back(all_nodes[j]);
            }
        }
    }
}

// =============================================================================
// GraphIndex Implementation
// =============================================================================

GraphIndex::GraphIndex(const GraphIndexConfig& config)
    : config_(config)
    , nav_data_(nullptr)
    , builder_(nullptr)
    , is_built_(false)
{
}

Error GraphIndex::build(const std::vector<Vector>& vectors) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (vectors.empty()) {
        return Error::invalid_argument("Empty vector dataset");
    }
    
    Size num_nodes = vectors.size();
    
    // Create navigation data
    nav_data_ = std::make_unique<GraphNavData>(num_nodes, config_.max_degree);
    
    // Create builder
    builder_ = std::make_unique<VamanaBuilder>(config_);
    
    // Build graph
    Error err = builder_->build(vectors, *nav_data_);
    if (!err.ok()) {
        return err;
    }
    
    vectors_ = vectors;  // Store for incremental insertion
    is_built_ = true;
    return Error::success();
}

Error GraphIndex::add_vector(const Vector& vector, NodeId node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_built_ || !nav_data_ || !builder_) {
        return Error::invalid_argument("Graph index not built");
    }

    // Append the new vector to our stored dataset
    if (node_id != vectors_.size()) {
        return Error::invalid_argument(
            "Node ID must equal current vector count for incremental insertion");
    }
    vectors_.push_back(vector);

    // Expand the graph navigation data to accommodate the new node
    NodeId new_id = nav_data_->add_node();
    if (new_id != node_id) {
        // Shouldn't happen if node_id == vectors_.size() - 1
        vectors_.pop_back();
        return Error::invalid_argument("Node ID mismatch after graph expansion");
    }

    // Use DiskANN-style incremental insertion: search + prune + reverse edges
    Error err = builder_->add_node_incremental(
        vectors_, vector, node_id, *nav_data_);
    if (!err.ok()) {
        // Rollback: remove the vector
        vectors_.pop_back();
        // Note: nav_data_ expansion is harder to rollback, but the node
        // will just have zero neighbors which is acceptable
        return err;
    }

    return Error::success();
}

const std::vector<NodeId>& GraphIndex::get_neighbors(NodeId node_id) const {
    if (!nav_data_) {
        throw StorageEngineException(
            ErrorCode::GRAPH_BUILD_FAILED,
            "Graph index not initialized"
        );
    }
    return nav_data_->get_neighbors(node_id);
}

NodeId GraphIndex::get_entry_point() const {
    if (!nav_data_) {
        return INVALID_NODE_ID;
    }
    return nav_data_->get_entry_point();
}

Size GraphIndex::get_num_nodes() const {
    if (!nav_data_) {
        return 0;
    }
    return nav_data_->get_num_nodes();
}

Size GraphIndex::get_memory_usage() const {
    if (!nav_data_) {
        return 0;
    }
    return nav_data_->calculate_memory_usage();
}

Error GraphIndex::save(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!nav_data_) {
        return Error::invalid_argument("Graph index not initialized");
    }
    
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return Error::io_error("Failed to open graph file for writing: " + filepath);
    }
    
    // Write header
    const char magic[] = "GRAPH001";
    file.write(magic, 8);
    
    Size num_nodes = nav_data_->get_num_nodes();
    file.write(reinterpret_cast<const char*>(&num_nodes), sizeof(num_nodes));
    
    Degree max_degree = nav_data_->get_max_degree();
    file.write(reinterpret_cast<const char*>(&max_degree), sizeof(max_degree));
    
    NodeId entry_point = nav_data_->get_entry_point();
    file.write(reinterpret_cast<const char*>(&entry_point), sizeof(entry_point));
    
    // Write adjacency lists
    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        const auto& neighbors = nav_data_->get_neighbors(i);
        Degree degree = static_cast<Degree>(neighbors.size());
        file.write(reinterpret_cast<const char*>(&degree), sizeof(degree));
        
        for (NodeId neighbor : neighbors) {
            file.write(reinterpret_cast<const char*>(&neighbor), sizeof(neighbor));
        }
    }
    
    file.close();
    return Error::success();
}

Error GraphIndex::load(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return Error::io_error("Failed to open graph file for reading: " + filepath);
    }
    
    // Read header
    char magic[8];
    file.read(magic, 8);
    if (std::string(magic) != "GRAPH001") {
        return Error::io_error("Invalid graph file format");
    }
    
    Size num_nodes;
    file.read(reinterpret_cast<char*>(&num_nodes), sizeof(num_nodes));
    
    Degree max_degree;
    file.read(reinterpret_cast<char*>(&max_degree), sizeof(max_degree));
    
    NodeId entry_point;
    file.read(reinterpret_cast<char*>(&entry_point), sizeof(entry_point));
    
    // Create navigation data
    nav_data_ = std::make_unique<GraphNavData>(num_nodes, max_degree);
    nav_data_->set_entry_point(entry_point);
    
    // Read adjacency lists
    for (NodeId i = 0; i < static_cast<NodeId>(num_nodes); ++i) {
        Degree degree;
        file.read(reinterpret_cast<char*>(&degree), sizeof(degree));
        
        auto& neighbors = nav_data_->get_neighbors_mut(i);
        neighbors.resize(degree);
        
        for (Degree j = 0; j < degree; ++j) {
            NodeId neighbor;
            file.read(reinterpret_cast<char*>(&neighbor), sizeof(neighbor));
            neighbors[j] = neighbor;
        }
    }
    
    file.close();
    is_built_ = true;
    return Error::success();
}

}  // namespace agent_mem_io