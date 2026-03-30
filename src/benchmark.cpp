/**
 * @file benchmark.cpp
 * @brief Complete benchmark for Agent-Mem-IO storage engine
 */

#include "common/types.h"
#include "data/synthetic_data.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>
#include <random>

using namespace agent_mem_io;

struct BenchmarkConfig {
    Size num_vectors = 10000;
    Dimension dimension = 128;
    Size num_queries = 100;
    Size k = 10;
    Size max_degree = 16;         // Balanced for recall and memory
    Size ef_construction = 200;   // Construction beam width
    Size ef_search = 250;         // Search beam width (higher = better recall)
};

template<typename Func>
double measure_time_ms(Func func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// L2 distance
float l2_dist(const Vector& a, const Vector& b) {
    float s = 0;
    for (Size i = 0; i < a.size() && i < b.size(); ++i) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return std::sqrt(s);
}

// Brute force search for ground truth
std::vector<NodeId> brute_force_search(const Vector& q, const std::vector<Vector>& base, Size k) {
    std::vector<std::pair<float, NodeId>> dists;
    for (Size i = 0; i < base.size(); ++i) {
        dists.push_back({l2_dist(q, base[i]), static_cast<NodeId>(i)});
    }
    std::partial_sort(dists.begin(), dists.begin() + std::min(k, dists.size()), dists.end());
    
    std::vector<NodeId> result;
    for (Size i = 0; i < k && i < dists.size(); ++i) {
        result.push_back(dists[i].second);
    }
    return result;
}

float calculate_recall(
    const std::vector<std::vector<NodeId>>& results,
    const std::vector<std::vector<NodeId>>& groundtruth,
    Size k
) {
    if (results.empty() || groundtruth.empty()) return 0.0f;
    
    Size total_hits = 0;
    Size total_relevant = 0;
    
    for (Size q = 0; q < results.size() && q < groundtruth.size(); ++q) {
        std::unordered_set<NodeId> gt_set;
        Size gt_k = std::min(k, static_cast<Size>(groundtruth[q].size()));
        for (Size i = 0; i < gt_k; ++i) {
            gt_set.insert(groundtruth[q][i]);
        }
        
        Size res_k = std::min(k, static_cast<Size>(results[q].size()));
        for (Size i = 0; i < res_k; ++i) {
            if (gt_set.count(results[q][i]) > 0) {
                total_hits++;
            }
        }
        total_relevant += gt_k;
    }
    
    return total_relevant > 0 ? static_cast<float>(total_hits) / total_relevant : 0.0f;
}

// NSW Graph Index
class NSWIndex {
public:
    NSWIndex(Size M, Size ef_c) : M_(M), ef_c_(ef_c) {}
    
    void build(const std::vector<Vector>& data) {
        data_ = &data;
        N_ = data.size();
        graph_.assign(N_, std::vector<NodeId>());
        
        // Insert nodes one by one
        for (NodeId i = 0; i < N_; ++i) {
            insert(i);
        }
    }
    
    std::vector<NodeId> search(const Vector& q, Size k, Size ef) {
        if (N_ == 0) return {};
        
        std::unordered_set<NodeId> visited;
        // Min-heap for candidates
        std::priority_queue<std::pair<float, NodeId>, 
                           std::vector<std::pair<float, NodeId>>,
                           std::greater<>> C;
        // Max-heap for results
        std::priority_queue<std::pair<float, NodeId>> W;
        
        // Start from random entry (or node 0)
        NodeId ep = 0;
        float d = l2_dist(q, (*data_)[ep]);
        C.push({d, ep});
        W.push({d, ep});
        visited.insert(ep);
        
        while (!C.empty()) {
            auto [cd, cn] = C.top();
            C.pop();
            
            float fd = W.top().first;  // furthest in W
            
            if (cd > fd && W.size() >= ef) break;
            
            for (NodeId n : graph_[cn]) {
                if (!visited.count(n)) {
                    visited.insert(n);
                    float nd = l2_dist(q, (*data_)[n]);
                    
                    if (W.size() < ef || nd < fd) {
                        C.push({nd, n});
                        W.push({nd, n});
                        
                        while (W.size() > ef) W.pop();
                        fd = W.top().first;
                    }
                }
            }
        }
        
        // Extract results sorted by distance
        std::vector<std::pair<float, NodeId>> temp;
        while (!W.empty()) {
            temp.push_back(W.top());
            W.pop();
        }
        std::sort(temp.begin(), temp.end());
        
        std::vector<NodeId> result;
        for (Size i = 0; i < k && i < temp.size(); ++i) {
            result.push_back(temp[i].second);
        }
        return result;
    }
    
private:
    void insert(NodeId q) {
        if (q == 0) return;  // First node has no neighbors yet
        
        // Find nearest neighbors
        std::vector<NodeId> neighbors = search_for_insert(q);
        
        // Select best neighbors using simple heuristic
        neighbors = select_neighbors(q, neighbors, M_);
        
        // Add bidirectional edges
        for (NodeId n : neighbors) {
            graph_[q].push_back(n);
            graph_[n].push_back(q);
            
            // Prune if too many edges
            if (graph_[n].size() > M_ * 2) {
                graph_[n] = select_neighbors(n, graph_[n], M_);
            }
        }
    }
    
    std::vector<NodeId> search_for_insert(NodeId q) {
        const Vector& vec = (*data_)[q];
        
        std::unordered_set<NodeId> visited;
        std::priority_queue<std::pair<float, NodeId>, 
                           std::vector<std::pair<float, NodeId>>,
                           std::greater<>> C;
        std::priority_queue<std::pair<float, NodeId>> W;
        
        // Start from node 0
        NodeId ep = 0;
        float d = l2_dist(vec, (*data_)[ep]);
        C.push({d, ep});
        W.push({d, ep});
        visited.insert(ep);
        
        while (!C.empty()) {
            auto [cd, cn] = C.top();
            C.pop();
            
            float fd = W.top().first;
            
            if (cd > fd && W.size() >= ef_c_) break;
            
            for (NodeId n : graph_[cn]) {
                if (n != q && !visited.count(n)) {
                    visited.insert(n);
                    float nd = l2_dist(vec, (*data_)[n]);
                    
                    if (W.size() < ef_c_ || nd < fd) {
                        C.push({nd, n});
                        W.push({nd, n});
                        
                        while (W.size() > ef_c_) W.pop();
                        fd = W.top().first;
                    }
                }
            }
        }
        
        std::vector<NodeId> result;
        while (!W.empty()) {
            result.push_back(W.top().second);
            W.pop();
        }
        return result;
    }
    
    std::vector<NodeId> select_neighbors(NodeId q, const std::vector<NodeId>& candidates, Size M) {
        std::vector<std::pair<float, NodeId>> sorted;
        for (NodeId c : candidates) {
            sorted.push_back({l2_dist((*data_)[q], (*data_)[c]), c});
        }
        std::sort(sorted.begin(), sorted.end());
        
        std::vector<NodeId> result;
        for (Size i = 0; i < M && i < sorted.size(); ++i) {
            result.push_back(sorted[i].second);
        }
        return result;
    }
    
    Size M_;
    Size ef_c_;
    Size N_ = 0;
    const std::vector<Vector>* data_;
    std::vector<std::vector<NodeId>> graph_;
};

void run_benchmark(const BenchmarkConfig& cfg) {
    std::cout << "========================================\n";
    std::cout << "Agent-Mem-IO Benchmark\n";
    std::cout << "========================================\n\n";
    
    std::cout << "Config: n=" << cfg.num_vectors << " d=" << cfg.dimension 
              << " q=" << cfg.num_queries << " k=" << cfg.k 
              << " M=" << cfg.max_degree << " ef=" << cfg.ef_search << "\n\n";
    
    // Generate data
    std::cout << "Generating clustered data...\n";
    std::mt19937 rng(42);
    std::normal_distribution<float> ndist(0.0f, 0.1f);
    std::uniform_real_distribution<float> udist(-1.0f, 1.0f);
    
    // Generate cluster centers
    Size num_clusters = 100;
    std::vector<Vector> centers(num_clusters);
    for (auto& c : centers) {
        c.resize(cfg.dimension);
        for (auto& v : c) v = udist(rng);
    }
    
    // Generate vectors around centers
    std::vector<Vector> base(cfg.num_vectors);
    std::uniform_int_distribution<int> cdist(0, num_clusters - 1);
    for (auto& v : base) {
        v = centers[cdist(rng)];
        for (auto& x : v) x += ndist(rng);
    }
    
    // Generate queries (pick random base vectors with small noise)
    std::cout << "Generating queries...\n";
    std::vector<Vector> queries(cfg.num_queries);
    std::uniform_int_distribution<int> qdist(0, cfg.num_vectors - 1);
    for (Size i = 0; i < cfg.num_queries; ++i) {
        queries[i] = base[qdist(rng)];
        for (auto& x : queries[i]) x += ndist(rng) * 0.5f;
    }
    
    // Compute ground truth with brute force
    std::cout << "Computing ground truth...\n";
    std::vector<std::vector<NodeId>> gt(cfg.num_queries);
    for (Size i = 0; i < cfg.num_queries; ++i) {
        gt[i] = brute_force_search(queries[i], base, cfg.k);
    }
    
    // Build index
    std::cout << "Building NSW index...\n";
    NSWIndex idx(cfg.max_degree, cfg.ef_construction);
    
    double build_t = measure_time_ms([&]() {
        idx.build(base);
    });
    std::cout << "  Build time: " << build_t << " ms\n\n";
    
    // Search
    std::cout << "Searching...\n";
    std::vector<std::vector<NodeId>> results(cfg.num_queries);
    double search_t = measure_time_ms([&]() {
        for (Size i = 0; i < cfg.num_queries; ++i) {
            results[i] = idx.search(queries[i], cfg.k, cfg.ef_search);
        }
    });
    std::cout << "  Search time: " << search_t << " ms\n";
    std::cout << "  QPS: " << (cfg.num_queries / search_t * 1000) << "\n\n";
    
    // Recall
    float recall = calculate_recall(results, gt, cfg.k);
    std::cout << "========================================\n";
    std::cout << "Recall@" << cfg.k << ": " << std::fixed << std::setprecision(2) << (recall * 100) << "%\n";
    std::cout << "Target: >= 85%\n";
    std::cout << "Status: " << (recall >= 0.85f ? "PASS ✓" : "FAIL ✗") << "\n\n";
    
    // Memory
    Size vec_mem = cfg.num_vectors * cfg.dimension * sizeof(float);
    Size graph_mem = cfg.num_vectors * cfg.max_degree * 2 * sizeof(NodeId);
    std::cout << "Memory: " << ((vec_mem + graph_mem) / 1024.0 / 1024.0) << " MB\n";
    std::cout << "Ratio: " << ((vec_mem + graph_mem) * 100.0 / vec_mem) << "%\n";
}

int main(int argc, char* argv[]) {
    BenchmarkConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-n" && i+1 < argc) cfg.num_vectors = std::stoul(argv[++i]);
        else if (a == "-d" && i+1 < argc) cfg.dimension = std::stoul(argv[++i]);
        else if (a == "-q" && i+1 < argc) cfg.num_queries = std::stoul(argv[++i]);
        else if (a == "-k" && i+1 < argc) cfg.k = std::stoul(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [-n num] [-d dim] [-q queries] [-k neighbors]\n";
            return 0;
        }
    }
    run_benchmark(cfg);
    return 0;
}