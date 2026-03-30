/**
 * @file main.cpp
 * @brief Main entry point for Agent-Mem-IO storage engine CLI
 */

#include "engine/storage_engine.h"
#include "common/types.h"
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace agent_mem_io;

// Command line argument parsing
struct CommandLineArgs {
    std::string command;
    std::string data_dir = "./data";
    std::string dataset_file;
    std::string query_file;
    std::string output_file;
    Size k = 10;
    Size num_queries = 0;
    bool verbose = false;
    bool help = false;
};

void print_help() {
    std::cout << "Agent-Mem-IO: Vector Retrieval Storage Engine for Agent Memory\n\n";
    std::cout << "Usage: agent_mem_io_cli <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  build       Build index from dataset\n";
    std::cout << "  search      Search for nearest neighbors\n";
    std::cout << "  insert      Insert vectors into index\n";
    std::cout << "  benchmark   Run performance benchmark\n";
    std::cout << "  verify      Verify recall@10 >= 85%\n\n";
    std::cout << "Options:\n";
    std::cout << "  --data-dir <path>     Data directory (default: ./data)\n";
    std::cout << "  --dataset <path>      Dataset file path\n";
    std::cout << "  --query <path>        Query file path\n";
    std::cout << "  --output <path>       Output file path\n";
    std::cout << "  --k <num>             Number of neighbors (default: 10)\n";
    std::cout << "  --num-queries <num>   Number of queries to run\n";
    std::cout << "  --verbose             Enable verbose output\n";
    std::cout << "  --help                Show this help message\n";
}

CommandLineArgs parse_args(int argc, char* argv[]) {
    CommandLineArgs args;
    
    if (argc < 2) {
        args.help = true;
        return args;
    }
    
    args.command = argv[1];
    
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--data-dir") {
            if (i + 1 < argc) {
                args.data_dir = argv[++i];
            }
        } else if (arg == "--dataset") {
            if (i + 1 < argc) {
                args.dataset_file = argv[++i];
            }
        } else if (arg == "--query") {
            if (i + 1 < argc) {
                args.query_file = argv[++i];
            }
        } else if (arg == "--output") {
            if (i + 1 < argc) {
                args.output_file = argv[++i];
            }
        } else if (arg == "--k") {
            if (i + 1 < argc) {
                args.k = std::stoul(argv[++i]);
            }
        } else if (arg == "--num-queries") {
            if (i + 1 < argc) {
                args.num_queries = std::stoul(argv[++i]);
            }
        } else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        }
    }
    
    return args;
}

// Load SIFT format dataset
std::vector<Vector> load_sift_dataset(const std::string& filepath, Size& num_vectors, Dimension& dim) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open dataset file: " + filepath);
    }
    
    // Read header
    int n, d;
    file.read(reinterpret_cast<char*>(&n), sizeof(int));
    file.read(reinterpret_cast<char*>(&d), sizeof(int));
    
    num_vectors = static_cast<Size>(n);
    dim = static_cast<Dimension>(d);
    
    std::cout << "Loading dataset: " << n << " vectors, dimension " << d << "\n";
    
    std::vector<Vector> vectors(n);
    for (int i = 0; i < n; ++i) {
        vectors[i].resize(d);
        for (int j = 0; j < d; ++j) {
            unsigned char val;
            file.read(reinterpret_cast<char*>(&val), sizeof(unsigned char));
            vectors[i][j] = static_cast<float>(val);
        }
    }
    
    file.close();
    return vectors;
}

// Load SIFT format queries with ground truth
std::vector<Vector> load_sift_queries(const std::string& filepath, Size& num_queries, Dimension& dim) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open query file: " + filepath);
    }
    
    int n, d;
    file.read(reinterpret_cast<char*>(&n), sizeof(int));
    file.read(reinterpret_cast<char*>(&d), sizeof(int));
    
    num_queries = static_cast<Size>(n);
    dim = static_cast<Dimension>(d);
    
    std::vector<Vector> queries(n);
    for (int i = 0; i < n; ++i) {
        queries[i].resize(d);
        for (int j = 0; j < d; ++j) {
            unsigned char val;
            file.read(reinterpret_cast<char*>(&val), sizeof(unsigned char));
            queries[i][j] = static_cast<float>(val);
        }
    }
    
    file.close();
    return queries;
}

// Load ground truth for recall calculation
std::vector<std::vector<NodeId>> load_ground_truth(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open ground truth file: " + filepath);
    }
    
    int n, d;
    file.read(reinterpret_cast<char*>(&n), sizeof(int));
    file.read(reinterpret_cast<char*>(&d), sizeof(int));
    
    std::vector<std::vector<NodeId>> ground_truth(n);
    for (int i = 0; i < n; ++i) {
        ground_truth[i].resize(d);
        for (int j = 0; j < d; ++j) {
            int val;
            file.read(reinterpret_cast<char*>(&val), sizeof(int));
            ground_truth[i][j] = static_cast<NodeId>(val);
        }
    }
    
    file.close();
    return ground_truth;
}

// Calculate recall@k
double calculate_recall(const std::vector<SearchResults>& results,
                        const std::vector<std::vector<NodeId>>& ground_truth,
                        Size k) {
    if (results.size() != ground_truth.size()) {
        return 0.0;
    }
    
    double total_recall = 0.0;
    for (Size i = 0; i < results.size(); ++i) {
        std::unordered_set<NodeId> result_set;
        for (Size j = 0; j < std::min(k, results[i].size()); ++j) {
            result_set.insert(results[i][j].node_id);
        }
        
        std::unordered_set<NodeId> truth_set;
        for (Size j = 0; j < std::min(k, ground_truth[i].size()); ++j) {
            truth_set.insert(ground_truth[i][j]);
        }
        
        Size intersection = 0;
        for (NodeId id : result_set) {
            if (truth_set.count(id) > 0) {
                intersection++;
            }
        }
        
        total_recall += static_cast<double>(intersection) / static_cast<double>(k);
    }
    
    return total_recall / static_cast<double>(results.size());
}

int run_build(const CommandLineArgs& args) {
    std::cout << "Building index from dataset: " << args.dataset_file << "\n";
    
    // Load dataset
    Size num_vectors;
    Dimension dim;
    auto vectors = load_sift_dataset(args.dataset_file, num_vectors, dim);
    
    std::cout << "Loaded " << num_vectors << " vectors\n";
    
    // Create storage engine
    StorageEngineConfig config;
    config.data_dir = args.data_dir;
    config.dimension = dim;
    
    // Set memory limit to 20% of dataset size
    Size dataset_size = num_vectors * dim * sizeof(float);
    config.memory_limit = static_cast<Size>(dataset_size * 0.2);
    
    std::cout << "Memory limit: " << config.memory_limit << " bytes ("
              << (config.memory_limit / 1024.0 / 1024.0) << " MB)\n";
    
    auto engine = std::make_unique<StorageEngine>(config);
    
    // Initialize
    Error err = engine->init();
    if (!err.ok()) {
        std::cerr << "Error initializing engine: " << err.message() << "\n";
        return 1;
    }
    
    // Load dataset
    err = engine->load_dataset(args.dataset_file, num_vectors);
    if (!err.ok()) {
        std::cerr << "Error loading dataset: " << err.message() << "\n";
        return 1;
    }
    
    // Build index
    std::cout << "Building graph index...\n";
    auto start = std::chrono::high_resolution_clock::now();
    
    err = engine->build_index();
    if (!err.ok()) {
        std::cerr << "Error building index: " << err.message() << "\n";
        return 1;
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Index built in " << duration.count() << " ms\n";
    
    // Save index
    err = engine->save();
    if (!err.ok()) {
        std::cerr << "Error saving index: " << err.message() << "\n";
        return 1;
    }
    
    std::cout << "Index saved to " << args.data_dir << "\n";
    
    // Print memory usage
    Size memory_usage = engine->get_memory_usage();
    std::cout << "Memory usage: " << memory_usage << " bytes ("
              << (memory_usage / 1024.0 / 1024.0) << " MB)\n";
    
    return 0;
}

int run_search(const CommandLineArgs& args) {
    std::cout << "Searching for nearest neighbors\n";
    
    // Create storage engine
    StorageEngineConfig config;
    config.data_dir = args.data_dir;
    
    auto engine = std::make_unique<StorageEngine>(config);
    
    // Initialize and load
    Error err = engine->init();
    if (!err.ok()) {
        std::cerr << "Error initializing engine: " << err.message() << "\n";
        return 1;
    }
    
    err = engine->load();
    if (!err.ok()) {
        std::cerr << "Error loading index: " << err.message() << "\n";
        return 1;
    }
    
    // Load queries
    Size num_queries;
    Dimension dim;
    auto queries = load_sift_queries(args.query_file, num_queries, dim);
    
    std::cout << "Loaded " << num_queries << " queries\n";
    
    // Run searches
    std::vector<SearchResults> all_results;
    std::vector<double> latencies;
    
    for (Size i = 0; i < num_queries; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        
        SearchResults results;
        err = engine->search(queries[i], args.k, results);
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        if (!err.ok()) {
            std::cerr << "Error searching query " << i << ": " << err.message() << "\n";
            continue;
        }
        
        all_results.push_back(results);
        latencies.push_back(duration.count() / 1000.0);
        
        if (args.verbose && i % 100 == 0) {
            std::cout << "Query " << i << ": latency " << latencies.back() << " ms\n";
        }
    }
    
    // Calculate statistics
    double total_latency = 0.0;
    for (double lat : latencies) {
        total_latency += lat;
    }
    double avg_latency = total_latency / latencies.size();
    
    // Sort latencies for P99
    std::sort(latencies.begin(), latencies.end());
    double p99_latency = latencies[static_cast<Size>(latencies.size() * 0.99)];
    
    std::cout << "\nSearch Statistics:\n";
    std::cout << "  Total queries: " << num_queries << "\n";
    std::cout << "  Average latency: " << std::fixed << std::setprecision(2) 
              << avg_latency << " ms\n";
    std::cout << "  P99 latency: " << p99_latency << " ms\n";
    std::cout << "  QPS: " << std::fixed << std::setprecision(1)
              << (1000.0 / avg_latency) << "\n";
    
    // Write results to output file
    if (!args.output_file.empty()) {
        std::ofstream out(args.output_file);
        for (Size i = 0; i < all_results.size(); ++i) {
            out << i << ":";
            for (const auto& result : all_results[i]) {
                out << " " << result.node_id;
            }
            out << "\n";
        }
        out.close();
        std::cout << "Results written to " << args.output_file << "\n";
    }
    
    return 0;
}

int run_verify(const CommandLineArgs& args) {
    std::cout << "Verifying recall@10 >= 85%\n";
    
    // Create storage engine
    StorageEngineConfig config;
    config.data_dir = args.data_dir;
    
    auto engine = std::make_unique<StorageEngine>(config);
    
    // Initialize and load
    Error err = engine->init();
    if (!err.ok()) {
        std::cerr << "Error initializing engine: " << err.message() << "\n";
        return 1;
    }
    
    err = engine->load();
    if (!err.ok()) {
        std::cerr << "Error loading index: " << err.message() << "\n";
        return 1;
    }
    
    // Load queries
    Size num_queries;
    Dimension dim;
    auto queries = load_sift_queries(args.query_file, num_queries, dim);
    
    // Load ground truth
    std::string gt_file = args.query_file;
    // Replace "query" with "groundtruth" in filename
    size_t pos = gt_file.find("query");
    if (pos != std::string::npos) {
        gt_file.replace(pos, 5, "groundtruth");
    }
    
    auto ground_truth = load_ground_truth(gt_file);
    
    std::cout << "Loaded " << num_queries << " queries with ground truth\n";
    
    // Run searches
    std::vector<SearchResults> all_results;
    for (Size i = 0; i < num_queries; ++i) {
        SearchResults results;
        err = engine->search(queries[i], args.k, results);
        if (!err.ok()) {
            std::cerr << "Error searching query " << i << "\n";
            continue;
        }
        all_results.push_back(results);
    }
    
    // Calculate recall
    double recall = calculate_recall(all_results, ground_truth, args.k);
    
    std::cout << "\nRecall@" << args.k << ": " << std::fixed << std::setprecision(4)
              << recall << "\n";
    
    // Check memory usage
    Size memory_usage = engine->get_memory_usage();
    Size dataset_size = engine->get_num_vectors() * dim * sizeof(float);
    double memory_ratio = static_cast<double>(memory_usage) / static_cast<double>(dataset_size);
    
    std::cout << "Memory usage: " << memory_usage << " bytes\n";
    std::cout << "Memory ratio: " << std::fixed << std::setprecision(2)
              << (memory_ratio * 100.0) << "% of dataset size\n";
    
    // Verify constraints
    bool recall_ok = recall >= MIN_RECALL_AT_10;
    bool memory_ok = memory_ratio >= MEMORY_LIMIT_RATIO_MIN && 
                     memory_ratio <= MEMORY_LIMIT_RATIO_MAX;
    
    std::cout << "\n=== Verification Results ===\n";
    std::cout << "Recall@10 >= 85%: " << (recall_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "Memory 10%-20%: " << (memory_ok ? "PASS" : "FAIL") << "\n";
    
    if (recall_ok && memory_ok) {
        std::cout << "\n*** ALL CONSTRAINTS SATISFIED ***\n";
        return 0;
    } else {
        std::cout << "\n*** CONSTRAINTS NOT SATISFIED ***\n";
        return 1;
    }
}

int run_benchmark(const CommandLineArgs& args) {
    std::cout << "Running performance benchmark\n";
    
    // TODO: Implement comprehensive benchmark
    std::cout << "Benchmark not yet implemented\n";
    return 0;
}

int main(int argc, char* argv[]) {
    try {
        CommandLineArgs args = parse_args(argc, argv);
        
        if (args.help) {
            print_help();
            return 0;
        }
        
        if (args.command == "build") {
            return run_build(args);
        } else if (args.command == "search") {
            return run_search(args);
        } else if (args.command == "verify") {
            return run_verify(args);
        } else if (args.command == "benchmark") {
            return run_benchmark(args);
        } else {
            std::cerr << "Unknown command: " << args.command << "\n";
            print_help();
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}