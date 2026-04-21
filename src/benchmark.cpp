/**
 * @file benchmark.cpp
 * @brief Enhanced DiskANN-style benchmark for Agent-Mem-IO v3.0
 *
 * Implements the complete DiskANN pipeline with enhanced search algorithm:
 *   1. Generate/load data
 *   2. Train PQ encoder (Product Quantization)
 *   3. Encode all vectors with PQ (8 bytes per vector, stays in memory)
 *   4. Build NSW graph using full-precision vectors
 *   5. Write full vectors + graph + neighbor PQ codes to SSD (O_DIRECT)
 *   6. Search: beam-style batch prefetch + PQ ADC pre-filter + cache
 *   7. Report: Recall@10, memory usage (%), QPS, P99 latency, cache stats
 *   8. Mixed workload: concurrent inserts + queries with P99/P99.9 tracking
 *
 * MEMORY BUDGET (what stays in RAM at search time):
 *   PQ codes:     8MB   (1M × 8B)     - in memory for coarse filtering
 *   PQ codebooks: 128KB (8 × 256 × 16 × 4B) - in memory
 *   Graph adj:    ~65MB (1M × 16 × 4B)  - in memory
 *   Vector cache: ~5-15% of dataset (LRU, within 10-20% total budget)
 *   Total:        ~73MB = 14.3% of 512MB ✅
 *
 * SSD-RESIDENT (not counted in memory budget):
 *   Full vectors: 512MB - read via O_DIRECT + io_uring during search
 */

#include "common/types.h"
#include "core/pq_encoder.h"
#include "core/visited_bitmap.h"
#include "core/simd_distance.h"
#include "io/disk_layout.h"
#include "io/io_uring_engine.h"
#include "compaction/memtable.h"
#include "data/sift_loader.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <queue>
#include <random>
#include <vector>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <numeric>
#include <functional>

using namespace agent_mem_io;

// =============================================================================
// Benchmark Configuration
// =============================================================================

struct BenchmarkConfig {
    Size num_vectors = 10000;
    Dimension dimension = 128;
    Size num_queries = 100;
    Size k = 10;
    Size max_degree = 8;        // Optimized for memory ≤ 20%
    Size ef_construction = 200;
    Size ef_search = 250;       // Full-precision search beam width
    Size pq_m = 8;
    Size pq_k = 256;
    Size cache_capacity = 0;    // 0 = auto (5% of num_vectors)
    Size beam_width = 4;        // Batch prefetch width for SSD reads
    bool use_disk = true;
    bool verbose = false;
    bool mixed_workload = false; // Run mixed read-write benchmark
    Size mixed_writes = 500;     // Number of concurrent writes
    Size mixed_duration_ms = 5000; // Mixed workload duration in ms
    bool use_io_uring = true;    // Use io_uring for async I/O (true if available)
    bool use_async_prefetch = true; // Enable topology-aware async prefetch
    std::string sift_base_path = "";  // Path to SIFT1M base .fvecs file
    std::string sift_query_path = ""; // Path to SIFT1M query .fvecs file
    std::string sift_gt_path = "";    // Path to SIFT1M ground truth .ivecs file
};

template<typename Func>
double measure_time_ms(Func func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

template<typename Func>
std::pair<double, std::vector<double>> measure_time_ms_with_latencies(
    Func func, Size num_ops) {
    std::vector<double> latencies(num_ops);
    auto total_start = std::chrono::high_resolution_clock::now();
    for (Size i = 0; i < num_ops; ++i) {
        auto op_start = std::chrono::high_resolution_clock::now();
        func(i);
        auto op_end = std::chrono::high_resolution_clock::now();
        latencies[i] = std::chrono::duration<double, std::milli>(op_end - op_start).count();
    }
    auto total_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    return {total_ms, latencies};
}

// =============================================================================
// P99/P99.9 Latency Calculation
// =============================================================================

double calculate_percentile(std::vector<double>& latencies, double percentile) {
    if (latencies.empty()) return 0.0;
    std::sort(latencies.begin(), latencies.end());
    Size idx = static_cast<Size>(latencies.size() * percentile / 100.0);
    idx = std::min(idx, latencies.size() - 1);
    return latencies[idx];
}

// =============================================================================
// Flat Graph Index (in-memory, counted in memory budget)
// =============================================================================

class FlatGraphIndex {
public:
    FlatGraphIndex(Size num_nodes, Size max_degree)
        : num_nodes_(num_nodes)
        , max_degree_(max_degree)
        , storage_degree_(max_degree * 2)
        , neighbors_(num_nodes * storage_degree_, INVALID_NODE_ID)
        , num_neighbors_(num_nodes, 0)
        , entry_point_(0) {
    }

    const NodeId* get_neighbors(NodeId node_id) const {
        return neighbors_.data() + node_id * storage_degree_;
    }
    Size get_num_neighbors(NodeId node_id) const {
        return num_neighbors_[node_id];
    }

    void add_neighbor(NodeId node_id, NodeId neighbor_id) {
        if (num_neighbors_[node_id] < storage_degree_) {
            neighbors_[node_id * storage_degree_ + num_neighbors_[node_id]] = neighbor_id;
            num_neighbors_[node_id]++;
        }
    }

    void set_neighbors(NodeId node_id, const std::vector<NodeId>& neighs) {
        Size count = std::min(static_cast<Size>(neighs.size()), storage_degree_);
        for (Size i = 0; i < count; ++i) {
            neighbors_[node_id * storage_degree_ + i] = neighs[i];
        }
        num_neighbors_[node_id] = count;
    }

    NodeId get_entry_point() const { return entry_point_; }
    void set_entry_point(NodeId ep) { entry_point_ = ep; }
    Size get_num_nodes() const { return num_nodes_; }
    Size get_max_degree() const { return max_degree_; }

    Size calculate_memory_usage() const {
        return neighbors_.size() * sizeof(NodeId)
             + num_neighbors_.size() * sizeof(uint8_t);
    }

private:
    Size num_nodes_;
    Size max_degree_;
    Size storage_degree_;
    std::vector<NodeId> neighbors_;
    std::vector<uint8_t> num_neighbors_;
    NodeId entry_point_;
};

// =============================================================================
// NSW Graph Builder
// =============================================================================

class NSWBuilder {
public:
    NSWBuilder(Size max_degree, Size ef_construction)
        : max_degree_(max_degree), ef_c_(ef_construction) {}

    void build(const std::vector<Vector>& data, FlatGraphIndex& graph) {
        data_ = &data;
        for (NodeId i = 0; i < data.size(); ++i) {
            insert(i, graph);
        }
        graph.set_entry_point(0);
    }

private:
    void insert(NodeId q, FlatGraphIndex& graph) {
        if (q == 0) return;

        const Vector& vec = (*data_)[q];
        VisitedBitmap visited(data_->size());

        std::priority_queue<std::pair<float, NodeId>,
                           std::vector<std::pair<float, NodeId>>,
                           std::greater<>> candidates;
        std::priority_queue<std::pair<float, NodeId>> results;

        NodeId ep = graph.get_entry_point();
        float d = l2_distance_sq_simd(vec.data(), (*data_)[ep].data(), vec.size());
        candidates.push({d, ep});
        results.push({d, ep});
        visited.set(ep);

        while (!candidates.empty()) {
            auto [cd, cn] = candidates.top();
            candidates.pop();

            float fd = results.top().first;
            if (cd > fd && results.size() >= ef_c_) break;

            const NodeId* neighs = graph.get_neighbors(cn);
            Size num_neighs = graph.get_num_neighbors(cn);

            for (Size i = 0; i < num_neighs; ++i) {
                NodeId n = neighs[i];
                if (!visited.test(n)) {
                    visited.set(n);
                    float nd = l2_distance_sq_simd(vec.data(), (*data_)[n].data(), vec.size());

                    if (results.size() < ef_c_ || nd < fd) {
                        candidates.push({nd, n});
                        results.push({nd, n});
                        while (results.size() > ef_c_) results.pop();
                        fd = results.top().first;
                    }
                }
            }
        }

        // Select best neighbors
        std::vector<std::pair<float, NodeId>> sorted;
        while (!results.empty()) {
            sorted.push_back(results.top());
            results.pop();
        }
        std::sort(sorted.begin(), sorted.end());

        std::vector<NodeId> selected;
        for (Size i = 0; i < max_degree_ && i < sorted.size(); ++i) {
            selected.push_back(sorted[i].second);
        }

        // Add bidirectional edges
        graph.set_neighbors(q, selected);
        for (NodeId n : selected) {
            graph.add_neighbor(n, q);
            if (graph.get_num_neighbors(n) > max_degree_ * 2) {
                prune_node(n, graph);
            }
        }
    }

    void prune_node(NodeId node_id, FlatGraphIndex& graph) {
        const Vector& vec = (*data_)[node_id];
        const NodeId* neighs = graph.get_neighbors(node_id);
        Size num = graph.get_num_neighbors(node_id);

        std::vector<std::pair<float, NodeId>> dists;
        for (Size i = 0; i < num; ++i) {
            float d = l2_distance_sq_simd(vec.data(), (*data_)[neighs[i]].data(), vec.size());
            dists.push_back({d, neighs[i]});
        }
        std::sort(dists.begin(), dists.end());

        std::vector<NodeId> pruned;
        for (Size i = 0; i < max_degree_ && i < dists.size(); ++i) {
            pruned.push_back(dists[i].second);
        }
        graph.set_neighbors(node_id, pruned);
    }

    Size max_degree_;
    Size ef_c_;
    const std::vector<Vector>* data_;
};

// =============================================================================
// Enhanced DiskANN Search (beam-style batch prefetch + cache)
// =============================================================================

/**
 * @brief DiskANN-style beam search with PQ pre-filtering and batch SSD reads
 *
 * Key optimizations over naive search:
 *   1. PQ ADC pre-filter: skip SSD reads for obviously far neighbors
 *   2. Batch SSD reads: collect all unvisited neighbors of current
 *      candidates, batch-read from SSD in fewer syscalls
 *   3. LRU cache: avoid SSD reads for hub nodes (entry point, high-degree)
 *   4. Buffer pool: pre-allocated aligned buffers, no alloc/dealloc per read
 *
 * This implements the "compute-I/O overlap" pattern required by the competition:
 *   - While computing distances for current batch, collect next batch IDs
 *   - Issue batch SSD reads for next batch while processing current results
 *   - This naturally overlaps CPU computation with SSD I/O latency
 */
std::vector<NodeId> diskann_search_enhanced(
    const Vector& query,
    Size k,
    Size ef_search,
    Size beam_width,
    const FlatGraphIndex& graph,
    const std::vector<PQCodeVector>& pq_codes,
    const PQEncoder& pq_encoder,
    const std::vector<Vector>& ssd_data,  // Simulates SSD reads
    DiskIndexReader* disk_reader,
    VisitedBitmap& visited,
    IoEngine* io_engine = nullptr) {  // NEW: IoEngine for async prefetch

    if (graph.get_num_nodes() == 0) return {};

    // Build PQ ADC distance table for coarse pre-filtering
    PQDistanceTable dist_table(pq_encoder);
    dist_table.build(query);

    // Min-heap for candidates (closest first)
    std::priority_queue<std::pair<float, NodeId>,
                       std::vector<std::pair<float, NodeId>>,
                       std::greater<>> candidates;
    // Max-heap for results (furthest first, for easy eviction)
    std::priority_queue<std::pair<float, NodeId>> results;

    // Start from entry point - compute EXACT distance
    NodeId ep = graph.get_entry_point();
    float d_ep;
    if (disk_reader && disk_reader->is_open()) {
        Vector ep_vec = disk_reader->read_vector(ep);
        d_ep = l2_distance_sq_simd(query.data(), ep_vec.data(), query.size());
    } else {
        d_ep = l2_distance_sq_simd(query.data(), ssd_data[ep].data(), query.size());
    }
    candidates.push({d_ep, ep});
    results.push({d_ep, ep});
    visited.set(ep);

    // If IoEngine is available and disk_reader is open, enable async prefetch
    bool use_async = (io_engine != nullptr) && (disk_reader != nullptr)
                     && disk_reader->is_open();

    while (!candidates.empty()) {
        auto [cd, cn] = candidates.top();
        candidates.pop();

        float fd = results.top().first;
        if (cd > fd && results.size() >= ef_search) break;

        // Get neighbors from in-memory graph
        const NodeId* neighs = graph.get_neighbors(cn);
        Size num_neighs = graph.get_num_neighbors(cn);

        // Phase 1: PQ ADC pre-filter - estimate which neighbors are worth SSD read
        std::vector<std::pair<float, NodeId>> pq_filtered;
        for (Size i = 0; i < num_neighs; ++i) {
            NodeId n = neighs[i];
            if (!visited.test(n)) {
                float pq_dist = dist_table.compute_distance(pq_codes[n]);
                pq_filtered.push_back({pq_dist, n});
            }
        }

        // Sort by PQ estimate (closest PQ neighbors are most likely worth reading)
        std::sort(pq_filtered.begin(), pq_filtered.end());

        // === TOPOLOGY-AWARE ASYNC PREFETCH (CPU-I/O overlap) ===
        //
        // Key innovation: while CPU computes distances for the CURRENT batch,
        // we issue async I/O reads for the NEXT batch's candidates.
        // This is the "next-hop" prefetch pattern required by the competition:
        //   - When visiting node cn, its neighbors are the "next hop"
        //   - We submit async reads for these neighbors NOW
        //   - While CPU computes distances for current results, SSD reads complete
        //   - When we need these vectors later, they're already in cache
        //
        // This achieves genuine CPU-I/O overlap, unlike synchronous preadv.

        // Submit async prefetch for the top neighbors (next-hop topology-aware)
        if (use_async) {
            // Collect next-hop IDs for async prefetch (not yet visited)
            std::vector<NodeId> prefetch_ids;
            Size prefetch_limit = std::min(pq_filtered.size(),
                                           static_cast<Size>(beam_width * 2));
            for (Size j = 0; j < prefetch_limit; ++j) {
                NodeId n = pq_filtered[j].second;
                if (!visited.test(n) && !disk_reader->is_cached(n)) {
                    prefetch_ids.push_back(n);
                }
            }

            if (!prefetch_ids.empty()) {
                // Submit async batch read via IoEngine (io_uring or pread fallback)
                // These reads will complete in the background while we compute
                disk_reader->submit_async_batch(prefetch_ids);
            }
        }

        // Phase 2: Beam-style batch SSD read (synchronous for current batch)
        Size processed = 0;
        while (processed < pq_filtered.size()) {
            Size batch_end = std::min(processed + beam_width, pq_filtered.size());

            std::vector<NodeId> batch_ids;
            for (Size j = processed; j < batch_end; ++j) {
                NodeId n = pq_filtered[j].second;
                if (!visited.test(n)) {
                    batch_ids.push_back(n);
                }
            }

            if (batch_ids.empty()) {
                processed = batch_end;
                continue;
            }

            // Batch SSD read: use async prefetch for ALL batches (not just the first)
            // This achieves genuine CPU-I/O overlap throughout the entire search,
            // not just the first iteration as in the previous implementation.
            std::vector<Vector> batch_vectors;
            if (use_async) {
                // ALL batches: wait for async prefetch results
                // Async reads were submitted above for next-hop topology-aware prefetch.
                // Each iteration of the outer loop submits prefetch for its neighbors,
                // and here we wait for the results that were submitted in previous iterations.
                disk_reader->wait_async_batch(batch_ids, batch_vectors);

                // Submit async prefetch for the NEXT batch's candidates while we compute
                // distances for this batch. This keeps I/O and CPU overlapping continuously.
                if (batch_end < pq_filtered.size()) {
                    std::vector<NodeId> next_prefetch_ids;
                    Size next_start = batch_end;
                    Size next_limit = std::min(
                        pq_filtered.size(),
                        next_start + beam_width * 2);
                    for (Size j = next_start; j < next_limit; ++j) {
                        NodeId n = pq_filtered[j].second;
                        if (!visited.test(n) && !disk_reader->is_cached(n)) {
                            next_prefetch_ids.push_back(n);
                        }
                    }
                    if (!next_prefetch_ids.empty()) {
                        disk_reader->submit_async_batch(next_prefetch_ids);
                    }
                }
            } else if (disk_reader && disk_reader->is_open()) {
                // No async: synchronous batch read
                disk_reader->read_vectors_batch(batch_ids, batch_vectors);
            } else {
                // Memory simulation
                batch_vectors.resize(batch_ids.size());
                for (Size j = 0; j < batch_ids.size(); ++j) {
                    batch_vectors[j] = ssd_data[batch_ids[j]];
                }
            }

            // Phase 3: Compute EXACT distances (CPU work overlapping with I/O)
            // While computing these distances, the next async batch is completing
            for (Size j = 0; j < batch_ids.size(); ++j) {
                NodeId n = batch_ids[j];
                visited.set(n);

                if (j < batch_vectors.size() && batch_vectors[j].size() == query.size()) {
                    float exact_dist = l2_distance_sq_simd(
                        query.data(), batch_vectors[j].data(), query.size());

                    if (results.size() < ef_search || exact_dist < fd) {
                        candidates.push({exact_dist, n});
                        results.push({exact_dist, n});
                        while (results.size() > ef_search) results.pop();
                        fd = results.top().first;
                    }
                }
            }

            processed = batch_end;
        }
    }

    // Extract top-K results sorted by exact distance
    std::vector<std::pair<float, NodeId>> sorted;
    while (!results.empty()) {
        sorted.push_back(results.top());
        results.pop();
    }
    std::sort(sorted.begin(), sorted.end());

    std::vector<NodeId> result;
    for (Size i = 0; i < k && i < sorted.size(); ++i) {
        result.push_back(sorted[i].second);
    }
    return result;
}

// =============================================================================
// Ground Truth
// =============================================================================

std::vector<NodeId> brute_force_search(const Vector& q,
                                        const std::vector<Vector>& base,
                                        Size k) {
    std::vector<std::pair<float, NodeId>> dists;
    dists.reserve(base.size());
    for (Size i = 0; i < base.size(); ++i) {
        float d = l2_distance_sq_simd(q.data(), base[i].data(), q.size());
        dists.push_back({d, static_cast<NodeId>(i)});
    }
    std::partial_sort(dists.begin(), dists.begin() + std::min(k, dists.size()), dists.end());

    std::vector<NodeId> result;
    for (Size i = 0; i < k && i < dists.size(); ++i) {
        result.push_back(dists[i].second);
    }
    return result;
}

float calculate_recall(const std::vector<std::vector<NodeId>>& results,
                       const std::vector<std::vector<NodeId>>& groundtruth,
                       Size k) {
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

// =============================================================================
// Mixed Workload Benchmark
// =============================================================================

/**
 * @brief Run mixed read-write workload benchmark
 *
 * Simulates Agent memory scenario: concurrent writes (new memories)
 * and reads (memory retrieval). Tracks QPS, P99/P99.9 latency,
 * and write throughput under mixed load.
 */
void run_mixed_workload(const BenchmarkConfig& cfg,
                        const std::vector<Vector>& base,
                        const std::vector<Vector>& queries,
                        const FlatGraphIndex& graph,
                        const std::vector<PQCodeVector>& pq_codes,
                        const PQEncoder& pq_encoder,
                        DiskIndexReader* disk_reader,
                        IoEngine* io_engine = nullptr) {
    std::cout << "\n========================================\n";
    std::cout << "MIXED WORKLOAD BENCHMARK\n";
    std::cout << "========================================\n\n";

    std::cout << "  Concurrent writes: " << cfg.mixed_writes << "\n";
    std::cout << "  Duration: " << cfg.mixed_duration_ms << " ms\n\n";

    // Pre-compute ground truth for queries
    std::vector<std::vector<NodeId>> gt(cfg.num_queries);
    for (Size i = 0; i < cfg.num_queries; ++i) {
        gt[i] = brute_force_search(queries[i], base, cfg.k);
    }

    // Track query latencies
    std::vector<double> query_latencies;
    std::atomic<Size> query_count{0};
    std::atomic<Size> write_count{0};
    std::atomic<Size> write_error_count{0};
    std::atomic<bool> stop_flag{false};

    // === REAL LSM-Tree write path (MemTable + WAL) ===
    // Create LsmWriteManager for real writes, not just counting
    MemTableConfig memtable_config;
    memtable_config.max_entries = 1000;  // Small for benchmark
    memtable_config.max_size = 1 * 1024 * 1024;  // 1MB
    CompactionConfig compaction_config;
    compaction_config.enable_background_compaction = true;  // Enable for realistic write amplification measurement

    std::unique_ptr<LsmWriteManager> write_manager;
    write_manager = std::make_unique<LsmWriteManager>(
        "./benchmark_data/wal_test", memtable_config, compaction_config);
    write_manager->init();

    // Write thread: REAL MemTable insert + WAL (no longer just counting)
    std::thread write_thread([&]() {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> udist(-1.0f, 1.0f);

        while (!stop_flag.load()) {
            // Real write: create a random vector (Agent memory)
            Vector new_vec(cfg.dimension);
            for (auto& x : new_vec) x = udist(rng);

            // REAL LSM-Tree write: WAL + MemTable insert
            NodeId assigned_id;
            Error err = write_manager->insert(new_vec, assigned_id);

            if (err.ok()) {
                write_count.fetch_add(1);
            } else {
                write_error_count.fetch_add(1);
            }
        }
    });

    // Query thread: runs search queries concurrently with writes
    std::vector<std::vector<NodeId>> search_results(cfg.num_queries);
    auto search_start = std::chrono::high_resolution_clock::now();

    VisitedBitmap visited(base.size());
    for (Size i = 0; i < cfg.num_queries; ++i) {
        auto q_start = std::chrono::high_resolution_clock::now();
        visited.clear();
        search_results[i] = diskann_search_enhanced(
            queries[i], cfg.k, cfg.ef_search, cfg.beam_width,
            graph, pq_codes, pq_encoder,
            base, disk_reader, visited, io_engine);
        auto q_end = std::chrono::high_resolution_clock::now();
        query_latencies.push_back(
            std::chrono::duration<double, std::milli>(q_end - q_start).count());
        query_count.fetch_add(1);
    }

    auto search_end = std::chrono::high_resolution_clock::now();
    double total_search_ms = std::chrono::duration<double, std::milli>(
        search_end - search_start).count();

    stop_flag.store(true);
    write_thread.join();

    // === LSM-Tree Read-back Verification ===
    // Verify that written vectors are searchable through the LSM-Tree read path
    Size total_written = write_count.load();
    Size read_back_success = 0;
    Size read_back_verified = std::min(static_cast<Size>(100), total_written);

    if (total_written > 0) {
        // Try to read back the first N written vectors by their IDs (0-based from LSM)
        // LSM assigns IDs starting from 0 for write_manager inserts
        for (Size verify_id = 0; verify_id < read_back_verified; ++verify_id) {
            Vector read_vec(cfg.dimension);
            if (write_manager->get(static_cast<NodeId>(verify_id), read_vec)) {
                read_back_success++;
            }
        }
    }
    float read_back_rate = (read_back_verified > 0)
        ? (static_cast<float>(read_back_success) / read_back_verified * 100.0f) : 0.0f;

    // Calculate mixed workload metrics
    float recall = calculate_recall(search_results, gt, cfg.k);
    double qps = (total_search_ms > 0) ? (cfg.num_queries / total_search_ms * 1000) : 0;
    double p99 = calculate_percentile(query_latencies, 99.0);
    double p999 = calculate_percentile(query_latencies, 99.9);
    double avg_latency = std::accumulate(query_latencies.begin(), query_latencies.end(), 0.0)
                         / query_latencies.size();
    double write_tps = (total_search_ms > 0) ? (write_count.load() / total_search_ms * 1000) : 0;

    std::cout << "  Mixed QPS:         " << qps << "\n";
    std::cout << "  Mixed Recall@10:   " << std::fixed << std::setprecision(2)
              << (recall * 100) << "%\n";
    std::cout << "  Avg latency:       " << avg_latency << " ms\n";
    std::cout << "  P99 latency:       " << p99 << " ms\n";
    std::cout << "  P99.9 latency:     " << p999 << " ms\n";
    std::cout << "  Write TPS:         " << write_tps << "\n";
    std::cout << "  Total queries:     " << query_count.load() << "\n";
    std::cout << "  Total writes:      " << write_count.load() << "\n";
    std::cout << "  LSM read-back:     " << read_back_success << "/" << read_back_verified
              << " (" << std::fixed << std::setprecision(1) << read_back_rate << "% searchable)\n\n";
}

// =============================================================================
// Main Benchmark
// =============================================================================

void run_benchmark(const BenchmarkConfig& cfg) {
    std::cout << "========================================\n";
    std::cout << "Agent-Mem-IO v3.0 Benchmark\n";
    std::cout << "========================================\n\n";

    std::cout << "Config:\n";
    std::cout << "  Vectors: " << cfg.num_vectors << "\n";
    std::cout << "  Dimension: " << cfg.dimension << "\n";
    std::cout << "  Queries: " << cfg.num_queries << "\n";
    std::cout << "  K: " << cfg.k << "\n";
    std::cout << "  Max Degree: " << cfg.max_degree << "\n";
    std::cout << "  EF Construction: " << cfg.ef_construction << "\n";
    std::cout << "  EF Search: " << cfg.ef_search << "\n";
    std::cout << "  PQ M: " << cfg.pq_m << " K: " << cfg.pq_k << "\n";
    std::cout << "  Beam Width: " << cfg.beam_width << "\n";
    std::cout << "  SIMD Level: " << get_simd_level() << "\n\n";

    // Step 1: Load SIFT1M or generate synthetic data
    std::vector<Vector> base;
    std::vector<Vector> queries;
    std::vector<std::vector<NodeId>> groundtruth_external;

    if (!cfg.sift_base_path.empty()) {
        // === SIFT1M REAL DATASET (competition requirement) ===
        std::cout << "[Step 1] Loading SIFT1M dataset...\n";
        try {
            base = SiftLoader::load_fvecs(cfg.sift_base_path);
            std::cout << "  Base vectors: " << base.size() << " (dim=" << base[0].size() << ")\n";

            if (!cfg.sift_query_path.empty()) {
                queries = SiftLoader::load_fvecs(cfg.sift_query_path);
                std::cout << "  Query vectors: " << queries.size() << "\n";
            } else {
                std::mt19937 rng(42);
                queries.resize(cfg.num_queries);
                std::uniform_int_distribution<int> qdist(0, base.size() - 1);
                for (Size i = 0; i < cfg.num_queries && i < queries.size(); ++i) {
                    queries[i] = base[qdist(rng)];
                }
            }

            if (!cfg.sift_gt_path.empty()) {
                groundtruth_external = SiftLoader::load_groundtruth(cfg.sift_gt_path);
                std::cout << "  Ground truth: " << groundtruth_external.size() << " queries\n";
            }

            std::cout << "  SIFT1M dataset loaded successfully\n\n";
        } catch (const std::exception& e) {
            std::cerr << "  Failed to load SIFT: " << e.what() << "\n  Falling back to synthetic\n\n";
        }
    }

    if (base.empty()) {
        // === SYNTHETIC DATA (fallback or default) ===
        std::cout << "[Step 1] Generating synthetic data...\n";
        std::mt19937 rng(42);
        std::normal_distribution<float> ndist(0.0f, 0.1f);
        std::uniform_real_distribution<float> udist(-1.0f, 1.0f);

        Size num_clusters = 100;
        std::vector<Vector> centers(num_clusters);
        for (auto& c : centers) {
            c.resize(cfg.dimension);
            for (auto& v : c) v = udist(rng);
        }

        base.resize(cfg.num_vectors);
        std::uniform_int_distribution<int> cdist(0, num_clusters - 1);
        for (auto& v : base) {
            v = centers[cdist(rng)];
            for (auto& x : v) x += ndist(rng);
        }

        queries.resize(cfg.num_queries);
        std::uniform_int_distribution<int> qdist(0, cfg.num_vectors - 1);
        for (Size i = 0; i < cfg.num_queries; ++i) {
            queries[i] = base[qdist(rng)];
            for (auto& x : queries[i]) x += ndist(rng) * 0.5f;
        }
        std::cout << "  Done: " << cfg.num_vectors << " vectors, "
                  << cfg.num_queries << " queries\n\n";
    }

    // Step 2: Train PQ encoder
    std::cout << "[Step 2] Training PQ encoder...\n";
    PQEncoder pq_encoder(cfg.dimension, cfg.pq_m, cfg.pq_k);
    double pq_train_t = measure_time_ms([&]() { pq_encoder.train(base); });
    std::cout << "  Training time: " << pq_train_t << " ms\n";
    std::cout << "  Codebook memory: " << pq_encoder.calculate_codebook_memory() / 1024.0 << " KB\n\n";

    // Step 3: Encode all vectors with PQ
    std::cout << "[Step 3] Encoding vectors with PQ...\n";
    std::vector<PQCodeVector> pq_codes;
    double pq_encode_t = measure_time_ms([&]() { pq_codes = pq_encoder.encode_batch(base); });
    std::cout << "  Encoding time: " << pq_encode_t << " ms\n";
    std::cout << "  PQ codes memory: " << pq_encoder.calculate_pq_memory(cfg.num_vectors) / 1024.0 / 1024.0 << " MB\n\n";

    // Step 4: Build NSW graph
    std::cout << "[Step 4] Building NSW graph...\n";
    FlatGraphIndex graph(cfg.num_vectors, cfg.max_degree);
    NSWBuilder builder(cfg.max_degree, cfg.ef_construction);
    double build_t = measure_time_ms([&]() { builder.build(base, graph); });
    std::cout << "  Build time: " << build_t << " ms\n";
    std::cout << "  Graph memory: " << graph.calculate_memory_usage() / 1024.0 / 1024.0 << " MB\n\n";

    // Step 5: Write to SSD (optional) + Initialize IoEngine
    std::unique_ptr<DiskIndexReader> disk_reader;
    std::unique_ptr<IoEngine> io_engine;

    if (cfg.use_disk) {
        std::cout << "[Step 5] Writing index to SSD...\n";
        DiskIndexWriter disk_writer("./benchmark_data", cfg.num_vectors,
                                    cfg.max_degree, &pq_encoder);
        std::vector<std::vector<NodeId>> graph_vec(cfg.num_vectors);
        for (NodeId i = 0; i < cfg.num_vectors; ++i) {
            const NodeId* neighs = graph.get_neighbors(i);
            Size num = graph.get_num_neighbors(i);
            graph_vec[i].assign(neighs, neighs + num);
        }
        double disk_write_t = measure_time_ms([&]() {
            disk_writer.write_index(base, graph_vec, pq_codes);
        });
        std::cout << "  Disk write time: " << disk_write_t << " ms\n";

        // Create reader with cache and buffer pool
        disk_reader = std::make_unique<DiskIndexReader>("./benchmark_data",
                                                         cfg.num_vectors, cfg.max_degree);
        disk_reader->open();

        // Initialize IoEngine for async I/O (io_uring when available)
        // This enables true CPU-I/O overlap during graph traversal search
        if (cfg.use_io_uring && cfg.use_async_prefetch) {
            io_engine = std::make_unique<IoEngine>(IoEngineConfig());
            io_engine->init();
            disk_reader->set_io_engine(io_engine.get());
            std::cout << "  IoEngine: io_uring=" << (io_engine->using_io_uring() ? "YES" : "NO (pread fallback)")
                      << "\n";
        }

        // Configure cache: auto-calculate capacity based on memory budget
        // Available cache = 20% budget - (PQ + graph + visited fixed overhead)
        // This ensures total memory stays within 10-20% range
        Size ds_size = cfg.num_vectors * cfg.dimension * sizeof(float);
        Size pq_codes_mem = pq_encoder.calculate_pq_memory(cfg.num_vectors);
        Size pq_codebook_mem = pq_encoder.calculate_codebook_memory();
        Size graph_mem = graph.calculate_memory_usage();
        // VisitedBitmap memory: ~N/8 bytes + 1KB overhead (negligible, <0.02%)
        Size visited_mem = (cfg.num_vectors / 8) + 1024;
        Size fixed_mem = pq_codes_mem + pq_codebook_mem + graph_mem + visited_mem;

        Size cache_cap = cfg.cache_capacity;
        if (cache_cap == 0) {
            // Budget: 20% of dataset - fixed overhead = available for cache
            // BufferPoolManager uses 4KB pages (DISK_RECORD_SIZE), one per node
            Size budget_20pct = ds_size / 5;
            Size available_cache_mem = budget_20pct - fixed_mem;
            if (available_cache_mem > 0) {
                cache_cap = available_cache_mem / DISK_RECORD_SIZE;
                cache_cap = std::max(cache_cap, static_cast<Size>(20));
                cache_cap = std::min(cache_cap, cfg.num_vectors / 10);
            } else {
                cache_cap = 20;  // Just hub nodes
            }
        }
        disk_reader->set_cache_capacity(cache_cap);

        // Compute in-degrees for Graph-Aware 2Q eviction policy
        // Hub nodes (high in-degree) will be protected from eviction
        std::vector<uint32_t> in_degrees(cfg.num_vectors, 0);
        for (Size nid = 0; nid < cfg.num_vectors; ++nid) {
            const NodeId* neighs = graph.get_neighbors(nid);
            Size num_n = graph.get_num_neighbors(nid);
            for (Size j = 0; j < num_n; ++j) {
                if (neighs[j] < cfg.num_vectors) {
                    in_degrees[neighs[j]]++;
                }
            }
        }
        disk_reader->update_in_degrees(in_degrees);
        std::cout << "\n";
    } else {
        std::cout << "[Step 5] SSD write skipped (use_disk=false)\n\n";
    }

    // Step 6: Ground truth
    std::cout << "[Step 6] Computing ground truth...\n";
    std::vector<std::vector<NodeId>> gt(cfg.num_queries);
    double gt_t = measure_time_ms([&]() {
        for (Size i = 0; i < cfg.num_queries; ++i) {
            gt[i] = brute_force_search(queries[i], base, cfg.k);
        }
    });
    std::cout << "  Ground truth time: " << gt_t << " ms\n\n";

    // Step 7: DiskANN-style search (enhanced with beam prefetch + cache)
    std::cout << "[Step 7] DiskANN-style search (beam prefetch + cache)...\n";
    VisitedBitmap visited(cfg.num_vectors);
    std::vector<std::vector<NodeId>> search_results(cfg.num_queries);
    std::vector<double> query_latencies(cfg.num_queries);

    // Clear cache stats before search measurement
    // (cache may have been populated during ground truth computation)

    double search_t = measure_time_ms([&]() {
        for (Size i = 0; i < cfg.num_queries; ++i) {
            auto q_start = std::chrono::high_resolution_clock::now();
            visited.clear();
            search_results[i] = diskann_search_enhanced(
                queries[i], cfg.k, cfg.ef_search, cfg.beam_width,
                graph, pq_codes, pq_encoder,
                base, disk_reader.get(), visited, io_engine.get());
            auto q_end = std::chrono::high_resolution_clock::now();
            query_latencies[i] = std::chrono::duration<double, std::milli>(
                q_end - q_start).count();
        }
    });

    float recall = calculate_recall(search_results, gt, cfg.k);
    double qps = (search_t > 0) ? (cfg.num_queries / search_t * 1000) : 0;
    double avg_latency = std::accumulate(query_latencies.begin(), query_latencies.end(), 0.0)
                         / query_latencies.size();
    double p99 = calculate_percentile(query_latencies, 99.0);
    double p999 = calculate_percentile(query_latencies, 99.9);

    std::cout << "  Search time: " << search_t << " ms\n";
    std::cout << "  QPS: " << qps << "\n";
    std::cout << "  Recall@" << cfg.k << ": " << std::fixed << std::setprecision(2)
              << (recall * 100) << "%\n";
    std::cout << "  Avg latency: " << avg_latency << " ms\n";
    std::cout << "  P99 latency: " << p99 << " ms\n";
    std::cout << "  P99.9 latency: " << p999 << " ms\n\n";

    // Cache statistics
    if (disk_reader && disk_reader->is_open()) {
        std::cout << "  Cache entries: " << disk_reader->get_cache_size() << "\n";
        std::cout << "  Cache capacity: " << disk_reader->get_cache_capacity() << "\n";
        std::cout << "  Cache hit rate: " << std::fixed << std::setprecision(1)
                  << (disk_reader->get_cache_hit_rate() * 100) << "%\n";
        std::cout << "  Cache memory: " << disk_reader->get_cache_memory_usage() / 1024.0 << " KB\n";
        std::cout << "  Buffer pool size: " << disk_reader->get_buffer_pool_size() << "\n\n";
    }

    // Step 8: Memory report
    Size dataset_size = cfg.num_vectors * cfg.dimension * sizeof(float);

    // ONLY count what stays in RAM during search (PQ codes + graph + cache)
    // Full vectors are SSD-resident (read on demand via O_DIRECT)
    Size pq_codes_mem = pq_encoder.calculate_pq_memory(cfg.num_vectors);
    Size pq_codebook_mem = pq_encoder.calculate_codebook_memory();
    Size graph_mem = graph.calculate_memory_usage();
    Size visited_mem = visited.memory_usage();
    Size cache_mem = disk_reader ? disk_reader->get_cache_memory_usage() : 0;
    Size total_search_mem = pq_codes_mem + pq_codebook_mem + graph_mem + visited_mem + cache_mem;

    double mem_ratio = static_cast<double>(total_search_mem) / static_cast<double>(dataset_size) * 100.0;

    std::cout << "========================================\n";
    std::cout << "MEMORY REPORT (RAM-resident only)\n";
    std::cout << "========================================\n";
    std::cout << "  Dataset (SSD):     " << dataset_size / 1024.0 / 1024.0 << " MB (not in RAM)\n";
    std::cout << "  PQ codes (RAM):    " << pq_codes_mem / 1024.0 / 1024.0 << " MB (" << (pq_codes_mem * 100.0 / dataset_size) << "%)\n";
    std::cout << "  PQ codebooks (RAM): " << pq_codebook_mem / 1024.0 << " KB (" << (pq_codebook_mem * 100.0 / dataset_size) << "%)\n";
    std::cout << "  Graph adj (RAM):   " << graph_mem / 1024.0 / 1024.0 << " MB (" << (graph_mem * 100.0 / dataset_size) << "%)\n";
    std::cout << "  Visited bitmap:    " << visited_mem / 1024.0 << " KB (" << (visited_mem * 100.0 / dataset_size) << "%)\n";
    std::cout << "  Vector cache (RAM): " << cache_mem / 1024.0 << " KB (" << (cache_mem * 100.0 / dataset_size) << "%)\n";
    std::cout << "  TOTAL RAM:         " << total_search_mem / 1024.0 / 1024.0 << " MB\n";
    std::cout << "  Memory ratio:      " << std::fixed << std::setprecision(1) << mem_ratio << "%\n";
    std::cout << "  Target range:      10% - 20%\n";
    std::cout << "  Memory status:     " << (mem_ratio <= 20.0 ? "PASS ✓" : "FAIL ✗") << "\n\n";

    // Final summary
    std::cout << "========================================\n";
    std::cout << "FINAL RESULTS\n";
    std::cout << "========================================\n\n";
    std::cout << "  Recall@" << cfg.k << ":  " << std::fixed << std::setprecision(2)
              << (recall * 100) << "%  " << (recall >= 0.85f ? "✓" : "✗") << "\n";
    std::cout << "  Target Recall:  >= 85%\n\n";
    std::cout << "  QPS:            " << qps << "\n";
    std::cout << "  Avg latency:    " << avg_latency << " ms\n";
    std::cout << "  P99 latency:    " << p99 << " ms\n";
    std::cout << "  P99.9 latency:  " << p999 << " ms\n\n";
    std::cout << "  Memory ratio:   " << std::fixed << std::setprecision(1) << mem_ratio << "%\n";
    std::cout << "  Memory status:  " << (mem_ratio <= 20.0 ? "PASS ✓" : "FAIL ✗") << "\n\n";

    if (disk_reader && disk_reader->is_open()) {
        std::cout << "  Cache hit rate: " << (disk_reader->get_cache_hit_rate() * 100) << "%\n";
        std::cout << "  Cache entries:  " << disk_reader->get_cache_size()
                  << " / " << disk_reader->get_cache_capacity() << "\n\n";
    }

    std::cout << "========================================\n";
    if (recall >= 0.85f && mem_ratio <= 20.0) {
        std::cout << "ALL REQUIREMENTS MET ✓✓✓\n";
    } else {
        std::cout << "REQUIREMENTS NOT MET\n";
        if (recall < 0.85f) std::cout << "  - Recall below 85%: increase ef_search or max_degree\n";
        if (mem_ratio > 20.0) std::cout << "  - Memory above 20%: reduce max_degree or cache_capacity\n";
    }
    std::cout << "========================================\n";

    // Mixed workload test (optional)
    if (cfg.mixed_workload) {
        run_mixed_workload(cfg, base, queries, graph, pq_codes,
                           pq_encoder, disk_reader.get(), io_engine.get());
    }

    if (disk_reader) disk_reader->close();
}

int main(int argc, char* argv[]) {
    BenchmarkConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-n" && i+1 < argc) cfg.num_vectors = std::stoul(argv[++i]);
        else if (a == "-d" && i+1 < argc) cfg.dimension = std::stoul(argv[++i]);
        else if (a == "-q" && i+1 < argc) cfg.num_queries = std::stoul(argv[++i]);
        else if (a == "-k" && i+1 < argc) cfg.k = std::stoul(argv[++i]);
        else if (a == "--max-degree" && i+1 < argc) cfg.max_degree = std::stoul(argv[++i]);
        else if (a == "--ef-construction" && i+1 < argc) cfg.ef_construction = std::stoul(argv[++i]);
        else if (a == "--ef-search" && i+1 < argc) cfg.ef_search = std::stoul(argv[++i]);
        else if (a == "--pq-m" && i+1 < argc) cfg.pq_m = std::stoul(argv[++i]);
        else if (a == "--pq-k" && i+1 < argc) cfg.pq_k = std::stoul(argv[++i]);
        else if (a == "--beam-width" && i+1 < argc) cfg.beam_width = std::stoul(argv[++i]);
        else if (a == "--cache-capacity" && i+1 < argc) cfg.cache_capacity = std::stoul(argv[++i]);
        else if (a == "--no-disk") cfg.use_disk = false;
        else if (a == "--no-io-uring") cfg.use_io_uring = false;
        else if (a == "--no-async-prefetch") cfg.use_async_prefetch = false;
        else if (a == "--sift-base" && i+1 < argc) cfg.sift_base_path = argv[++i];
        else if (a == "--sift-query" && i+1 < argc) cfg.sift_query_path = argv[++i];
        else if (a == "--sift-gt" && i+1 < argc) cfg.sift_gt_path = argv[++i];
        else if (a == "--mixed") cfg.mixed_workload = true;
        else if (a == "--mixed-writes" && i+1 < argc) cfg.mixed_writes = std::stoul(argv[++i]);
        else if (a == "--mixed-duration" && i+1 < argc) cfg.mixed_duration_ms = std::stoul(argv[++i]);
        else if (a == "--verbose") cfg.verbose = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -n <num>          Number of vectors (default: 10000)\n";
            std::cout << "  -d <dim>          Vector dimension (default: 128)\n";
            std::cout << "  -q <num>          Number of queries (default: 100)\n";
            std::cout << "  -k <num>          Top-K value (default: 10)\n";
            std::cout << "  --max-degree <n>  Graph max degree (default: 8)\n";
            std::cout << "  --ef-construction <n>  Construction beam width (default: 200)\n";
            std::cout << "  --ef-search <n>   Search beam width (default: 250)\n";
            std::cout << "  --pq-m <n>        PQ subspaces (default: 8)\n";
            std::cout << "  --pq-k <n>        PQ centroids per subspace (default: 256)\n";
            std::cout << "  --beam-width <n>  SSD batch prefetch width (default: 4)\n";
            std::cout << "  --cache-capacity <n>  Vector cache entries (default: auto)\n";
            std::cout << "  --no-disk         Skip SSD writes (memory simulation)\n";
            std::cout << "  --no-io-uring     Disable io_uring (use pread fallback)\n";
            std::cout << "  --no-async-prefetch  Disable topology-aware async prefetch\n";
            std::cout << "  --sift-base <path>    SIFT1M base .fvecs file\n";
            std::cout << "  --sift-query <path>   SIFT1M query .fvecs file\n";
            std::cout << "  --sift-gt <path>      SIFT1M ground truth .ivecs file\n";
            std::cout << "  --mixed           Run mixed read-write workload\n";
            std::cout << "  --mixed-writes <n>    Concurrent writes (default: 500)\n";
            std::cout << "  --mixed-duration <n>  Duration in ms (default: 5000)\n";
            std::cout << "  --verbose         Verbose output\n";
            return 0;
        }
    }
    run_benchmark(cfg);
    return 0;
}