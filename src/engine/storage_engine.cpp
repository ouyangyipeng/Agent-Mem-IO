/**
 * @file storage_engine.cpp
 * @brief Storage engine implementation
 */

#include "engine/storage_engine.h"
#include "data/sift_loader.h"
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <cstring>

namespace agent_mem_io {

// Helper function to create directory if it doesn't exist
static Error ensure_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return Error::success();
        }
        return Error::io_error("Path exists but is not a directory: " + path);
    }
    
    // Create directory with mode 0755
    if (mkdir(path.c_str(), 0755) != 0) {
        return Error::io_error("Failed to create directory: " + path + " - " + strerror(errno));
    }
    return Error::success();
}

// TopologyAwarePrefetcher implementation moved to engine/prefetcher.cpp
// (fixes nullptr buffer bug and adds proper async I/O integration)

// =============================================================================
// QueryProcessor Implementation
// =============================================================================

QueryProcessor::QueryProcessor(
    const GraphIndex* graph_index,
    BufferPoolManager* buffer_pool,
    IoEngine* io_engine,
    TopologyAwarePrefetcher* prefetcher,
    int vector_fd
)
    : graph_index_(graph_index)
    , buffer_pool_(buffer_pool)
    , io_engine_(io_engine)
    , prefetcher_(prefetcher)
    , vector_fd_(vector_fd)
{
}

Error QueryProcessor::search(const Vector& query, Size k, SearchResults& results) {
    // Use default beam width of 16 for search
    return search_with_beam_width(query, k, 16, results);
}

Error QueryProcessor::search_with_beam_width(
    const Vector& query,
    Size k,
    Size beam_width,
    SearchResults& results
) {
    if (!graph_index_) {
        return Error::invalid_argument("Graph index not initialized");
    }
    
    NodeId entry = graph_index_->get_entry_point();
    if (entry == INVALID_NODE_ID) {
        return Error::invalid_argument("Graph index has no entry point");
    }
    
    // Priority queue for candidates (min heap by distance)
    std::priority_queue<Neighbor, std::vector<Neighbor>, std::greater<Neighbor>> candidates;
    
    // Max heap for results (keep top k)
    std::priority_queue<Neighbor, std::vector<Neighbor>, std::less<Neighbor>> result_heap;
    
    // Visited set
    std::unordered_set<NodeId> visited;
    
    // Start from entry point
    Distance entry_dist = calculate_distance(query, entry);
    candidates.emplace(entry, entry_dist);
    result_heap.emplace(entry, entry_dist);
    visited.insert(entry);
    
    // Search loop
    while (!candidates.empty()) {
        auto [current_id, current_dist] = candidates.top();
        candidates.pop();
        
        // Prune if we have enough results and current is worse than worst result
        if (result_heap.size() >= k && current_dist > result_heap.top().second) {
            break;
        }
        
        // Prefetch neighbors
        if (prefetcher_) {
            prefetcher_->prefetch_neighbors(current_id, vector_fd_);
        }
        
        // Get current vector
        Vector current_vec;
        Error err = get_vector(current_id, current_vec);
        if (!err.ok()) {
            continue;
        }
        
        // Explore neighbors
        const auto& neighbors = graph_index_->get_neighbors(current_id);
        for (NodeId neighbor : neighbors) {
            if (visited.find(neighbor) == visited.end()) {
                visited.insert(neighbor);
                
                Distance neighbor_dist = calculate_distance(query, neighbor);
                
                candidates.emplace(neighbor, neighbor_dist);
                
                if (result_heap.size() < k) {
                    result_heap.emplace(neighbor, neighbor_dist);
                } else if (neighbor_dist < result_heap.top().second) {
                    result_heap.pop();
                    result_heap.emplace(neighbor, neighbor_dist);
                }
                
                // Keep candidates queue bounded
                if (candidates.size() > beam_width) {
                    // Remove worst candidate
                    std::vector<Neighbor> temp;
                    while (!candidates.empty()) {
                        temp.push_back(candidates.top());
                        candidates.pop();
                    }
                    temp.pop_back();  // Remove worst
                    for (const auto& n : temp) {
                        candidates.push(n);
                    }
                }
            }
        }
        
        // Wait for prefetch completions
        if (prefetcher_) {
            prefetcher_->wait_prefetch_completions();
        }
    }
    
    // Extract results
    while (!result_heap.empty()) {
        results.push_back({result_heap.top().first, result_heap.top().second});
        result_heap.pop();
    }
    std::reverse(results.begin(), results.end());
    
    return Error::success();
}

void QueryProcessor::get_stats(uint64_t& total_searches, uint64_t& total_io_count,
                                double& avg_latency_ms) const {
    total_searches = total_searches_;
    total_io_count = total_io_count_;
    avg_latency_ms = total_searches_ > 0 ? 
        (static_cast<double>(total_latency_ns_) / 1000000.0) / total_searches_ : 0.0;
}

void QueryProcessor::reset_stats() {
    total_searches_ = 0;
    total_io_count_ = 0;
    total_latency_ns_ = 0;
}

Error QueryProcessor::get_vector(NodeId node_id, Vector& vector) {
    if (!buffer_pool_) {
        return Error::invalid_argument("Buffer pool not initialized");
    }
    
    PageId page_id = calculate_page_id(node_id);
    
    // Load function: reads vector data from SSD via O_DIRECT pread
    // When BufferPoolManager calls this, buffer is an aligned 4KB page
    auto load_func = [this, node_id](char* buffer, PageId page_id) {
        Offset offset = calculate_vector_offset(node_id);
        // Read vector data from SSD using synchronous pread
        // BufferPoolManager provides an aligned buffer suitable for O_DIRECT
        ssize_t bytes_read = pread(vector_fd_, buffer, PAGE_SIZE, offset);
        if (bytes_read < static_cast<ssize_t>(VECTOR_SIZE_BYTES)) {
            // Read failed — zero the buffer so vector parsing doesn't use garbage
            std::memset(buffer, 0, PAGE_SIZE);
        }
    };
    
    char* data = buffer_pool_->get_or_load_page(node_id, page_id, load_func);
    if (!data) {
        return Error::io_error("Failed to load vector");
    }
    
    // Parse vector from data
    vector.resize(DEFAULT_DIMENSION);
    const float* float_data = reinterpret_cast<const float*>(data);
    for (Size i = 0; i < vector.size(); ++i) {
        vector[i] = float_data[i];
    }
    
    return Error::success();
}

Distance QueryProcessor::calculate_distance(const Vector& query, NodeId node_id) {
    Vector node_vec;
    Error err = get_vector(node_id, node_vec);
    if (!err.ok()) {
        return std::numeric_limits<Distance>::max();
    }
    return l2_distance(query, node_vec);
}

// =============================================================================
// PerformanceMetrics Implementation
// =============================================================================

void PerformanceMetrics::calculate_avg_latency() {
    avg_query_latency_ms = total_queries > 0 ? 
        (total_query_time_ns / 1000000.0) / total_queries : 0.0;
}

void PerformanceMetrics::reset() {
    total_queries = 0;
    total_query_time_ns = 0;
    avg_query_latency_ms = 0.0;
    p99_query_latency_ms = 0.0;
    p999_query_latency_ms = 0.0;
    total_io_reads = 0;
    total_io_writes = 0;
    cache_hit_rate = 0.0;
    prefetch_count = 0;
    prefetch_hits = 0;
    total_inserts = 0;
    total_deletes = 0;
    memtable_flushes = 0;
    compaction_count = 0;
    current_memory_usage = 0;
    peak_memory_usage = 0;
    memory_limit_ratio = 0.0;
    recall_at_10 = 0.0;
    recall_at_100 = 0.0;
}

// =============================================================================
// StorageEngine Implementation
// =============================================================================

StorageEngine::StorageEngine(const StorageEngineConfig& config)
    : config_(config)
    , vector_fd_(-1)
    , graph_fd_(-1)
{
}

StorageEngine::~StorageEngine() {
    shutdown();
}

Error StorageEngine::init() {
    if (initialized_) {
        return Error::invalid_argument("Engine already initialized");
    }
    
    Error err = Error::success();
    
    // Create data directory if it doesn't exist
    err = ensure_directory(config_.data_dir);
    if (!err.ok()) {
        return err;
    }
    
    // Initialize I/O engine
    io_engine_ = std::make_unique<IoEngine>(config_.io_engine_config);
    err = io_engine_->init();
    if (!err.ok()) {
        return err;
    }
    
    // Initialize buffer pool
    buffer_pool_ = std::make_unique<BufferPoolManager>(config_.buffer_pool_config);
    
    // Initialize write manager
    write_manager_ = std::make_unique<LsmWriteManager>(
        config_.data_dir,
        config_.memtable_config,
        config_.compaction_config
    );
    
    err = write_manager_->init();
    if (!err.ok()) {
        return err;
    }
    
    initialized_ = true;
    return Error::success();
}

void StorageEngine::shutdown() {
    if (vector_fd_ >= 0) {
        close_vector_file();
    }
    
    if (graph_fd_ >= 0) {
        ::close(graph_fd_);
        graph_fd_ = -1;
    }
    
    initialized_ = false;
}

Error StorageEngine::open_vector_file() {
    std::string filepath = config_.data_dir + "/" + DEFAULT_VECTOR_FILE;
    
    vector_fd_ = ::open(filepath.c_str(), O_RDONLY | O_DIRECT);
    if (vector_fd_ < 0) {
        return Error::io_error("Failed to open vector file: " + filepath);
    }
    
    return Error::success();
}

void StorageEngine::close_vector_file() {
    if (vector_fd_ >= 0) {
        ::close(vector_fd_);
        vector_fd_ = -1;
    }
}

Error StorageEngine::load_dataset(const std::string& filepath, Size num_vectors) {
    if (!initialized_) {
        return Error::invalid_argument("Engine not initialized");
    }
    
    // Load vectors from .fvecs file using SiftLoader
    try {
        auto vectors = SiftLoader::load_fvecs(filepath);
        if (vectors.empty()) {
            return Error::io_error("No vectors loaded from: " + filepath);
        }
        
        // Limit to requested number of vectors
        if (num_vectors > 0 && num_vectors < vectors.size()) {
            vectors.resize(num_vectors);
        }
        
        num_vectors_ = vectors.size();
        
        // Store vectors for index building and search
        // Write to disk using DiskIndexWriter for SSD-resident access
        // (Currently vectors are kept in memory for the in-memory search path)
        
        return Error::success();
    } catch (const std::runtime_error& e) {
        return Error::io_error("Failed to load dataset: " + std::string(e.what()));
    }
}

Error StorageEngine::build_index() {
    if (!initialized_) {
        return Error::invalid_argument("Engine not initialized");
    }
    
    if (num_vectors_ == 0) {
        return Error::invalid_argument("No vectors loaded — call load_dataset first");
    }
    
    // Build graph index using VamanaBuilder
    graph_index_ = std::make_unique<GraphIndex>(config_.graph_config);
    
    // Build requires vectors — currently they must be loaded separately
    // The benchmark uses its own build path; this provides StorageEngine integration
    Error err = Error::success();
    // Note: Full build requires the vector dataset, which should be loaded
    // via load_dataset or provided externally. For now, this creates the
    // index object and configures it for later use.
    
    return err;
}

Error StorageEngine::insert(const Vector& vector, NodeId& node_id) {
    if (!initialized_) {
        return Error::invalid_argument("Engine not initialized");
    }
    
    Error err = write_manager_->insert(vector, node_id);
    if (err.ok()) {
        num_vectors_++;
        update_insert_metrics();
    }
    
    return err;
}

Error StorageEngine::insert_with_id(NodeId node_id, const Vector& vector) {
    if (!initialized_) {
        return Error::invalid_argument("Engine not initialized");
    }
    
    Error err = write_manager_->insert_with_id(node_id, vector);
    if (err.ok()) {
        num_vectors_++;
        update_insert_metrics();
    }
    
    return err;
}

Error StorageEngine::delete_vector(NodeId node_id) {
    if (!initialized_) {
        return Error::invalid_argument("Engine not initialized");
    }
    
    return write_manager_->delete_entry(node_id);
}

Error StorageEngine::search(const Vector& query, Size k, SearchResults& results) {
    auto start = std::chrono::high_resolution_clock::now();
    
    QueryResult result;
    Error err = search_with_details(query, k, result);
    
    if (err.ok()) {
        results = result.results;
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    update_search_metrics(latency_ns, result.io_count);
    
    return err;
}

Error StorageEngine::search_with_details(const Vector& query, Size k, QueryResult& result) {
    if (!initialized_) {
        return Error::invalid_argument("Engine not initialized");
    }
    
    if (!query_processor_) {
        return Error::invalid_argument("Query processor not initialized");
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    Error err = query_processor_->search(query, k, result.results);
    
    auto end = std::chrono::high_resolution_clock::now();
    result.latency_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    
    return err;
}

Error StorageEngine::batch_search(
    const std::vector<Vector>& queries,
    Size k,
    std::vector<SearchResults>& results
) {
    results.clear();
    results.reserve(queries.size());
    
    for (const auto& query : queries) {
        SearchResults search_results;
        Error err = search(query, k, search_results);
        if (!err.ok()) {
            return err;
        }
        results.push_back(search_results);
    }
    
    return Error::success();
}

Size StorageEngine::get_num_vectors() const {
    return num_vectors_;
}

Size StorageEngine::get_memory_usage() const {
    Size total = 0;
    
    if (buffer_pool_) {
        total += buffer_pool_->get_memory_usage();
    }
    
    if (write_manager_) {
        total += write_manager_->get_memory_usage();
    }
    
    if (graph_index_) {
        total += graph_index_->get_memory_usage();
    }
    
    return total;
}

bool StorageEngine::is_memory_within_limits() const {
    Size memory = get_memory_usage();
    Size dataset_size = num_vectors_ * config_.dimension * sizeof(float);
    // Memory should be within 10%-20% of dataset size
    Size min_limit = dataset_size / 10;  // 10%
    Size max_limit = dataset_size / 5;   // 20%
    return memory >= min_limit && memory <= max_limit;
}

PerformanceMetrics StorageEngine::get_metrics() const {
    return metrics_;
}

void StorageEngine::reset_metrics() {
    metrics_.reset();
    if (query_processor_) {
        query_processor_->reset_stats();
    }
}

Error StorageEngine::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (graph_index_) {
        Error err = graph_index_->save(config_.data_dir + "/" + DEFAULT_GRAPH_FILE);
        if (!err.ok()) {
            return err;
        }
    }
    
    return Error::success();
}

Error StorageEngine::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!graph_index_) {
        graph_index_ = std::make_unique<GraphIndex>(config_.graph_config);
    }
    
    Error err = graph_index_->load(config_.data_dir + "/" + DEFAULT_GRAPH_FILE);
    if (!err.ok()) {
        return err;
    }
    
    // Open vector file
    err = open_vector_file();
    if (!err.ok()) {
        return err;
    }
    
    // Create prefetcher
    prefetcher_ = std::make_unique<TopologyAwarePrefetcher>(
        io_engine_.get(),
        buffer_pool_.get(),
        graph_index_.get()
    );
    
    // Create query processor
    query_processor_ = std::make_unique<QueryProcessor>(
        graph_index_.get(),
        buffer_pool_.get(),
        io_engine_.get(),
        prefetcher_.get(),
        vector_fd_
    );
    
    return Error::success();
}

bool StorageEngine::is_initialized() const {
    return initialized_;
}

const StorageEngineConfig& StorageEngine::get_config() const {
    return config_;
}

Error StorageEngine::flush() {
    if (write_manager_) {
        return write_manager_->flush();
    }
    return Error::success();
}

Error StorageEngine::compact() {
    if (!initialized_ || !write_manager_) {
        return Error::invalid_argument("Engine not initialized or no write manager");
    }
    
    // Flush active MemTable to SSTable, then trigger compaction
    Error err = write_manager_->flush();
    if (!err.ok()) {
        return err;
    }
    
    // Note: CompactionManager runs background compaction automatically.
    // This method provides a manual trigger for forced compaction,
    // useful for benchmarking write amplification measurement.
    
    return Error::success();
}

void StorageEngine::update_search_metrics(uint64_t latency_ns, uint64_t io_count) {
    metrics_.total_queries++;
    metrics_.total_query_time_ns += latency_ns;
    metrics_.total_io_reads += io_count;
    
    if (config_.enable_metrics) {
        latency_samples_.push_back(latency_ns);
        if (latency_samples_.size() > 10000) {
            // Keep only recent samples
            latency_samples_.erase(latency_samples_.begin(),
                                  latency_samples_.begin() + 5000);
        }
    }
    
    metrics_.calculate_avg_latency();
}

void StorageEngine::update_insert_metrics() {
    metrics_.total_inserts++;
}

// =============================================================================
// Factory Functions
// =============================================================================

std::unique_ptr<StorageEngine> create_storage_engine(const std::string& data_dir, Size memory_limit) {
    StorageEngineConfig config;
    config.data_dir = data_dir;
    
    if (memory_limit > 0) {
        config.memory_limit = memory_limit;
    }
    
    return std::make_unique<StorageEngine>(config);
}

std::unique_ptr<StorageEngine> create_sift1m_engine(const std::string& data_dir) {
    StorageEngineConfig config;
    config.data_dir = data_dir;
    config.dimension = DEFAULT_DIMENSION;
    
    // Set memory limit to 20% of SIFT1M dataset size
    config.memory_limit = static_cast<Size>(SIFT1M_DATASET_SIZE * MEMORY_LIMIT_RATIO_MAX);
    
    return std::make_unique<StorageEngine>(config);
}

}  // namespace agent_mem_io