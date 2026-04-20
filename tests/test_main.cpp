/**
 * @file test_main.cpp
 * @brief Main test file for Agent-Mem-IO storage engine v3.0
 *
 * Comprehensive unit tests covering all core components:
 *   - Types and constants
 *   - Error handling (ErrorCode + Error class)
 *   - Distance functions (L2, inner product, cosine)
 *   - SIMD distance computation (AVX2/SSE)
 *   - Graph index (NSW / Vamana)
 *   - GraphNavData
 *   - Buffer pool manager
 *   - Product Quantization encoder + ADC distance table
 *   - VisitedBitmap
 *   - Disk layout (writer + reader + vector cache + buffer pool)
 *   - MemTable + LSM write manager
 *   - Storage engine
 */

#include "common/types.h"
#include "core/graph_index.h"
#include "core/pq_encoder.h"
#include "core/visited_bitmap.h"
#include "core/simd_distance.h"
#include "buffer/buffer_pool.h"
#include "io/disk_layout.h"
#include "io/io_uring_engine.h"
#include "compaction/memtable.h"
#include "engine/storage_engine.h"

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <chrono>
#include <random>
#include <cstring>
#include <filesystem>

using namespace agent_mem_io;

// =============================================================================
// Test: Distance Functions
// =============================================================================

void test_distance_functions() {
    std::cout << "Testing distance functions..." << std::endl;

    Vector v1 = {1.0f, 2.0f, 3.0f};
    Vector v2 = {4.0f, 5.0f, 6.0f};

    // L2 distance
    Distance l2d = l2_distance(v1, v2);
    assert(std::abs(l2d - 5.196152f) < 0.001f);

    // Inner product
    Distance ip = inner_product_distance(v1, v2);
    assert(std::abs(ip - (-32.0f)) < 0.001f);

    // Cosine distance
    Vector v3 = {1.0f, 0.0f, 0.0f};
    Vector v4 = {0.0f, 1.0f, 0.0f};
    Distance cd = cosine_distance(v3, v4);
    assert(std::abs(cd - 1.0f) < 0.001f);

    std::cout << "  Distance functions: PASSED" << std::endl;
}

// =============================================================================
// Test: SIMD Distance Functions
// =============================================================================

void test_simd_distance() {
    std::cout << "Testing SIMD distance functions..." << std::endl;

    // Test with 128-dim vectors (SIFT standard)
    Size dim = 128;
    std::vector<float> v1(dim), v2(dim);
    for (Size i = 0; i < dim; ++i) {
        v1[i] = static_cast<float>(i) * 0.01f;
        v2[i] = static_cast<float>(i + 1) * 0.01f;
    }

    // SIMD L2 squared distance
    float simd_dist = l2_distance_sq_simd(v1.data(), v2.data(), dim);

    // Compute reference L2 squared distance
    float ref_dist = 0.0f;
    for (Size i = 0; i < dim; ++i) {
        float diff = v1[i] - v2[i];
        ref_dist += diff * diff;
    }

    // SIMD should match reference within floating-point tolerance
    assert(std::abs(simd_dist - ref_dist) < ref_dist * 0.001f + 0.01f);

    // Test with zero vectors
    std::vector<float> zero(dim, 0.0f);
    float zd = l2_distance_sq_simd(v1.data(), zero.data(), dim);
    float ref_z = 0.0f;
    for (Size i = 0; i < dim; ++i) ref_z += v1[i] * v1[i];
    assert(std::abs(zd - ref_z) < ref_z * 0.001f + 0.01f);

    // Test SIMD level detection
    int level = get_simd_level();
    assert(level >= 0);  // At least scalar fallback
    std::cout << "    SIMD level: " << level << " (0=scalar, 1=SSE, 2=AVX2)\n";

    std::cout << "  SIMD distance: PASSED" << std::endl;
}

// =============================================================================
// Test: GraphNavData
// =============================================================================

void test_graph_nav_data() {
    std::cout << "Testing GraphNavData..." << std::endl;

    GraphNavData nav_data(100, 10);

    assert(nav_data.get_num_nodes() == 100);
    assert(nav_data.get_max_degree() == 10);

    nav_data.add_neighbor(0, 1);
    nav_data.add_neighbor(0, 2);
    nav_data.add_neighbor(0, 3);

    assert(nav_data.get_num_neighbors(0) == 3);
    assert(nav_data.get_neighbor(0, 0) == 1);
    assert(nav_data.get_neighbor(0, 1) == 2);
    assert(nav_data.get_neighbor(0, 2) == 3);

    std::cout << "  GraphNavData: PASSED" << std::endl;
}

// =============================================================================
// Test: Graph Index (Vamana)
// =============================================================================

void test_graph_index() {
    std::cout << "Testing GraphIndex..." << std::endl;

    GraphIndexConfig config;
    config.max_degree = 16;
    config.search_list_size = 200;

    GraphIndex index(config);

    // Add some vectors
    std::vector<Vector> data;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    for (Size i = 0; i < 100; ++i) {
        Vector v(128);
        for (auto& x : v) x = dist(rng);
        data.push_back(v);
    }

    // Build index
    Error err = index.build(data);
    assert(err.ok());

    assert(index.get_num_vectors() == 100);
    assert(index.get_entry_point() < 100);

    // Calculate memory
    Size mem = index.get_memory_usage();
    assert(mem > 0);

    std::cout << "  GraphIndex: PASSED" << std::endl;
}

// =============================================================================
// Test: Buffer Pool
// =============================================================================

void test_buffer_pool() {
    std::cout << "Testing BufferPoolManager..." << std::endl;

    BufferPoolConfig config;
    config.max_pages = 10;

    BufferPoolManager pool(config);

    int load_count = 0;
    auto load_func = [&load_count](char* buffer, PageId page_id) {
        load_count++;
        std::memset(buffer, 0, 4096);
        std::memcpy(buffer, &page_id, sizeof(PageId));
    };

    // Load some pages
    pool.get_or_load_page(0, 1, load_func);
    assert(load_count == 1);

    pool.get_or_load_page(0, 2, load_func);
    assert(load_count == 2);

    // Hit on existing page
    pool.get_or_load_page(0, 1, load_func);
    assert(load_count == 2);  // No new load

    // Check hit rate
    double hr = pool.get_hit_rate();
    assert(hr > 0.0);

    std::cout << "  BufferPoolManager: PASSED" << std::endl;
}

// =============================================================================
// Test: PQ Encoder
// =============================================================================

void test_pq_encoder() {
    std::cout << "Testing PQ Encoder..." << std::endl;

    Size dim = 128;
    Size num_vectors = 500;
    Size pq_m = 8;
    Size pq_k = 256;

    std::mt19937 rng(42);
    std::normal_distribution<float> ndist(0.0f, 1.0f);

    std::vector<Vector> data(num_vectors);
    for (auto& v : data) {
        v.resize(dim);
        for (auto& x : v) x = ndist(rng);
    }

    // Train PQ encoder (returns bool)
    PQEncoder encoder(dim, pq_m, pq_k);
    bool trained = encoder.train(data);
    assert(trained);

    // Encode a single vector
    PQCodeVector codes = encoder.encode(data[0]);
    assert(codes.size() == pq_m);

    // Encode batch
    std::vector<PQCodeVector> batch_codes = encoder.encode_batch(data);
    assert(batch_codes.size() == num_vectors);
    for (const auto& c : batch_codes) {
        assert(c.size() == pq_m);
    }

    // Verify codebook memory
    Size codebook_mem = encoder.calculate_codebook_memory();
    assert(codebook_mem > 0);
    assert(codebook_mem == pq_m * pq_k * (dim / pq_m) * sizeof(float));

    // Verify PQ codes memory
    Size pq_mem = encoder.calculate_pq_memory(num_vectors);
    assert(pq_mem == num_vectors * pq_m * sizeof(PQCode));

    // Test PQ ADC distance table
    Vector query(dim);
    for (auto& x : query) x = ndist(rng);

    PQDistanceTable dist_table(encoder);
    dist_table.build(query);

    float pd1 = dist_table.compute_distance(batch_codes[0]);
    assert(pd1 >= 0.0f);

    float pd2 = dist_table.compute_distance(batch_codes[0]);
    assert(std::abs(pd1 - pd2) < 0.001f);  // Reproducible

    std::cout << "  PQ Encoder: PASSED" << std::endl;
}

// =============================================================================
// Test: VisitedBitmap
// =============================================================================

void test_visited_bitmap() {
    std::cout << "Testing VisitedBitmap..." << std::endl;

    Size n = 10000;
    VisitedBitmap bitmap(n);

    // Initially all unvisited
    assert(!bitmap.test(0));
    assert(!bitmap.test(42));

    // Set some bits
    bitmap.set(0);
    bitmap.set(42);
    bitmap.set(9999);
    assert(bitmap.test(0));
    assert(bitmap.test(42));
    assert(bitmap.test(9999));
    assert(!bitmap.test(1));

    // Clear
    bitmap.clear();
    assert(!bitmap.test(0));
    assert(!bitmap.test(42));

    // Memory usage
    Size mem = bitmap.memory_usage();
    assert(mem > 0);
    assert(mem < n);  // Should be much smaller than n

    std::cout << "  VisitedBitmap: PASSED" << std::endl;
}

// =============================================================================
// Test: Disk Layout (Writer + Reader + Cache + Buffer Pool)
// =============================================================================

void test_disk_layout() {
    std::cout << "Testing Disk Layout..." << std::endl;

    Size num_vectors = 100;
    Size dim = 128;
    Size max_degree = 8;
    Size pq_m = 8;
    // PQ with K=16 needs fewer training samples than K=256
    Size pq_k = 16;  // Small K for unit test (256 needs >2048 samples)

    std::mt19937 rng(42);
    std::normal_distribution<float> ndist(0.0f, 1.0f);

    std::vector<Vector> data(num_vectors);
    for (auto& v : data) {
        v.resize(dim);
        for (auto& x : v) x = ndist(rng);
    }

    // Create PQ encoder and encode (small K=16 for unit test)
    PQEncoder pq_encoder(dim, pq_m, pq_k);
    pq_encoder.train(data);
    auto pq_codes = pq_encoder.encode_batch(data);

    // Create graph structure
    std::vector<std::vector<NodeId>> graph_vec(num_vectors);
    for (NodeId i = 0; i < num_vectors; ++i) {
        Size num_neigh = 2 + (i % 3);
        for (Size j = 0; j < num_neigh; ++j) {
            NodeId neigh = (i + j + 1) % num_vectors;
            graph_vec[i].push_back(neigh);
        }
    }

    // Write to disk
    std::string test_dir = "./test_disk_data";
    std::filesystem::remove_all(test_dir);

    DiskIndexWriter writer(test_dir, num_vectors, max_degree, &pq_encoder);
    Size bw = writer.write_index(data, graph_vec, pq_codes);
    assert(bw > 0);
    assert(bw == num_vectors * DISK_RECORD_SIZE);
    std::cout << "    Write: OK (" << bw << " bytes)\n";

    // Test VectorCache (separate from disk reader, no O_DIRECT issues)
    std::cout << "    Testing VectorCache...\n";
    VectorCache cache(50);
    assert(cache.capacity() == 50);
    assert(cache.size() == 0);

    // Zero-initialize DiskNodeRecord to avoid uninitialized fields
    DiskNodeRecord test_rec{};
    test_rec.node_id = 1;
    for (Size i = 0; i < dim; ++i) test_rec.vector_data[i] = data[1][i];
    test_rec.num_neighbors = 3;
    test_rec.neighbor_ids[0] = 2;
    test_rec.neighbor_ids[1] = 3;
    test_rec.neighbor_ids[2] = 4;

    cache.put(1, test_rec);
    assert(cache.has(1));
    assert(cache.size() == 1);

    const DiskNodeRecord* cached = cache.get(1);
    assert(cached != nullptr);
    assert(cached->node_id == 1);
    assert(cache.hit_count() == 1);
    assert(cache.miss_count() == 0);

    const DiskNodeRecord* miss_ptr = cache.get(999);
    assert(miss_ptr == nullptr);
    assert(cache.miss_count() == 1);

    // Cache eviction
    for (NodeId i = 0; i < 60; ++i) {
        DiskNodeRecord r{};  // Zero-initialize
        r.node_id = i;
        cache.put(i, r);
    }
    assert(cache.size() <= 50);

    Size cm = cache.memory_usage();
    assert(cm > 0);
    assert(cm <= 50 * VectorCache::ENTRY_MEMORY);
    std::cout << "    VectorCache: OK\n";

    // Read back from disk
    std::cout << "    Testing DiskIndexReader...\n";
    DiskIndexReader reader(test_dir, num_vectors, max_degree);
    assert(reader.open());

    // Test single vector read (cache-aware)
    Vector vec0 = reader.read_vector(0);
    assert(vec0.size() == dim);
    for (Size i = 0; i < dim; ++i) {
        assert(std::abs(vec0[i] - data[0][i]) < 0.001f);
    }
    std::cout << "    read_vector(0): OK\n";

    // Test single record read (cache-aware)
    DiskNodeRecord rec = reader.read_record(42);
    assert(rec.node_id == 42);
    for (Size i = 0; i < dim; ++i) {
        assert(std::abs(rec.vector_data[i] - data[42][i]) < 0.001f);
    }
    std::cout << "    read_record(42): OK\n";

    // Test batch read
    std::vector<NodeId> batch_ids = {0, 10, 20, 50, 99};
    std::vector<Vector> batch_vecs;
    Size rc = reader.read_vectors_batch(batch_ids, batch_vecs);
    assert(rc == batch_ids.size());
    for (Size j = 0; j < batch_ids.size(); ++j) {
        assert(batch_vecs[j].size() == dim);
        for (Size i = 0; i < dim; ++i) {
            assert(std::abs(batch_vecs[j][i] - data[batch_ids[j]][i]) < 0.001f);
        }
    }
    std::cout << "    read_vectors_batch: OK\n";

    // Buffer pool: borrow → read → parse → return
    char* buf = reader.borrow_buffer();
    assert(buf != nullptr);
    bool ok = reader.read_node(0, buf);
    assert(ok);
    DiskNodeRecord buf_rec = reader.parse_record(buf);
    assert(buf_rec.node_id == 0);
    reader.return_buffer(buf);
    std::cout << "    borrow_buffer + read_node + parse: OK\n";

    reader.close();

    // Cleanup
    std::filesystem::remove_all(test_dir);

    std::cout << "  Disk Layout: PASSED" << std::endl;
}

// =============================================================================
// Test: MemTable
// =============================================================================

void test_memtable() {
    std::cout << "Testing MemTable..." << std::endl;

    MemTableConfig config;
    config.max_size = 1024 * 1024;  // 1MB

    MemTable memtable(config);

    Vector v1 = {1.0f, 2.0f, 3.0f, 4.0f};
    Vector v2 = {5.0f, 6.0f, 7.0f, 8.0f};

    NodeId id1 = 0, id2 = 1;
    Error err = memtable.insert(id1, v1);
    assert(err.ok());

    err = memtable.insert(id2, v2);
    assert(err.ok());

    // Check memory usage
    Size mem = memtable.get_memory_usage();
    assert(mem > 0);

    std::cout << "  MemTable: PASSED" << std::endl;
}

// =============================================================================
// Test: LSM Write Manager
// =============================================================================

void test_lsm_write_manager() {
    std::cout << "Testing LsmWriteManager..." << std::endl;

    std::string test_dir = "/tmp/agent_mem_io_lsm_test";
    std::filesystem::create_directories(test_dir);

    MemTableConfig mt_config;
    mt_config.max_size = 1024 * 1024;  // 1MB

    LsmWriteManager manager(test_dir, mt_config);

    Vector v1 = {1.0f, 2.0f, 3.0f, 4.0f};
    NodeId id1;
    Error err = manager.insert(v1, id1);
    assert(err.ok());

    Vector v2 = {5.0f, 6.0f, 7.0f, 8.0f};
    NodeId id2;
    err = manager.insert(v2, id2);
    assert(err.ok());

    std::filesystem::remove_all(test_dir);

    std::cout << "  LsmWriteManager: PASSED (partial)" << std::endl;
}

// =============================================================================
// Test: Storage Engine
// =============================================================================

void test_storage_engine() {
    std::cout << "Testing StorageEngine..." << std::endl;

    StorageEngineConfig config;
    config.memory_limit = 100 * 1024 * 1024;  // 100MB
    config.data_dir = "/tmp/agent_mem_io_se_test";

    auto engine = create_storage_engine(config.data_dir, config.memory_limit);
    assert(engine != nullptr);

    // Check config
    const StorageEngineConfig& rc = engine->get_config();
    assert(rc.memory_limit == config.memory_limit);

    std::cout << "  StorageEngine: PASSED" << std::endl;
}

// =============================================================================
// Test: Types and Constants
// =============================================================================

void test_types_and_constants() {
    std::cout << "Testing types and constants..." << std::endl;

    assert(sizeof(NodeId) == 4);
    assert(sizeof(Dimension) == 4);
    assert(sizeof(Distance) == 4);
    assert(sizeof(Size) == 8);
    assert(sizeof(Offset) == 8);
    assert(sizeof(PageId) == 8);

    assert(DEFAULT_DIMENSION == 128);
    assert(DEFAULT_MAX_DEGREE == 16);
    assert(INVALID_NODE_ID == std::numeric_limits<NodeId>::max());
    assert(ALIGNMENT == 4096);
    assert(DEFAULT_PQ_M == 8);

    std::cout << "  Types and constants: PASSED" << std::endl;
}

// =============================================================================
// Test: Error Handling
// =============================================================================

void test_error_handling() {
    std::cout << "Testing error handling..." << std::endl;

    // Test ErrorCode enum
    assert(ErrorCode::SUCCESS != ErrorCode::IO_ERROR);
    assert(ErrorCode::IO_ERROR != ErrorCode::UNKNOWN_ERROR);

    // Test Error class
    Error ok_err = Error::success();
    assert(ok_err.ok());
    assert(ok_err.code() == ErrorCode::SUCCESS);

    Error io_err = Error::io_error("test io error");
    assert(!io_err.ok());
    assert(io_err.code() == ErrorCode::IO_ERROR);
    assert(io_err.message() == "test io error");

    // Test SearchResult
    SearchResult result;
    result.node_id = 42;
    result.distance = 3.14f;
    assert(result.node_id == 42);
    assert(std::abs(result.distance - 3.14f) < 0.001f);

    std::cout << "  Error handling: PASSED" << std::endl;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Agent-Mem-IO Unit Tests v3.0" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        test_types_and_constants();
        test_error_handling();
        test_distance_functions();
        test_simd_distance();
        test_graph_nav_data();
        test_graph_index();
        test_buffer_pool();
        test_pq_encoder();
        test_visited_bitmap();
        test_disk_layout();
        test_memtable();
        test_lsm_write_manager();
        test_storage_engine();

        std::cout << "========================================" << std::endl;
        std::cout << "All tests PASSED!" << std::endl;
        std::cout << "========================================" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test FAILED with exception: " << e.what() << std::endl;
        return 1;
    }
}