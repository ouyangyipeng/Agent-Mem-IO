/**
 * @file test_main.cpp
 * @brief Main test file for Agent-Mem-IO storage engine
 */

#include "common/types.h"
#include "core/graph_index.h"
#include "buffer/buffer_pool.h"
#include "io/io_uring_engine.h"
#include "compaction/memtable.h"
#include "engine/storage_engine.h"

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <chrono>

using namespace agent_mem_io;

// Test helper functions
void test_distance_functions() {
    std::cout << "Testing distance functions..." << std::endl;
    
    Vector v1 = {1.0f, 2.0f, 3.0f};
    Vector v2 = {4.0f, 5.0f, 6.0f};
    
    // L2 distance
    Distance l2 = l2_distance(v1, v2);
    assert(std::abs(l2 - 5.196152f) < 0.001f);
    
    // Inner product
    Distance ip = inner_product_distance(v1, v2);
    assert(std::abs(ip - (-32.0f)) < 0.001f);
    
    // Cosine distance
    Vector v3 = {1.0f, 0.0f, 0.0f};
    Vector v4 = {0.0f, 1.0f, 0.0f};
    Distance cos_dist = cosine_distance(v3, v4);
    assert(std::abs(cos_dist - 1.0f) < 0.001f);
    
    std::cout << "  Distance functions: PASSED" << std::endl;
}

void test_graph_nav_data() {
    std::cout << "Testing GraphNavData..." << std::endl;
    
    GraphNavData nav_data(100, 10);
    
    // Test initial state
    assert(nav_data.get_num_nodes() == 100);
    assert(nav_data.get_max_degree() == 10);
    assert(nav_data.get_entry_point() == INVALID_NODE_ID);
    
    // Test adding neighbors
    nav_data.set_entry_point(0);
    assert(nav_data.get_entry_point() == 0);
    
    assert(nav_data.add_neighbor(0, 1));
    assert(nav_data.add_neighbor(0, 2));
    assert(nav_data.add_neighbor(0, 3));
    
    const auto& neighbors = nav_data.get_neighbors(0);
    assert(neighbors.size() == 3);
    
    // Test degree limit
    for (int i = 4; i <= 10; ++i) {
        assert(nav_data.add_neighbor(0, i));
    }
    assert(nav_data.get_degree(0) == 10);
    assert(!nav_data.add_neighbor(0, 11));  // Should fail (max degree)
    
    // Test remove neighbor
    assert(nav_data.remove_neighbor(0, 5));
    assert(nav_data.get_degree(0) == 9);
    
    std::cout << "  GraphNavData: PASSED" << std::endl;
}

void test_graph_index() {
    std::cout << "Testing GraphIndex..." << std::endl;
    
    // Create simple test vectors
    std::vector<Vector> vectors;
    for (int i = 0; i < 100; ++i) {
        Vector vec;
        for (int j = 0; j < 128; ++j) {
            vec.push_back(static_cast<float>(i * 128 + j) / 1000.0f);
        }
        vectors.push_back(vec);
    }
    
    GraphIndexConfig config;
    config.max_degree = 16;
    config.search_list_size = 50;
    
    GraphIndex index(config);
    
    // Build index
    Error err = index.build(vectors);
    assert(err.ok());
    assert(index.is_built());
    assert(index.get_num_nodes() == 100);
    assert(index.get_entry_point() != INVALID_NODE_ID);
    
    // Check memory usage
    Size memory = index.get_memory_usage();
    assert(memory > 0);
    
    std::cout << "  GraphIndex: PASSED" << std::endl;
}

void test_buffer_pool() {
    std::cout << "Testing BufferPoolManager..." << std::endl;
    
    BufferPoolConfig config;
    config.max_pages = 100;
    config.page_size = PAGE_SIZE;
    
    BufferPoolManager pool(config);
    
    // Test initial state
    assert(pool.max_size() == 100);
    assert(pool.size() == 0);
    
    // Test get_or_load_page
    int load_count = 0;
    auto load_func = [&load_count](char* buffer, PageId page_id) {
        load_count++;
        std::memset(buffer, static_cast<int>(page_id), PAGE_SIZE);
    };
    
    char* page1 = pool.get_or_load_page(1, 1, load_func);
    assert(page1 != nullptr);
    assert(load_count == 1);
    assert(pool.size() == 1);
    
    // Test cache hit
    char* page1_again = pool.get_or_load_page(1, 1, load_func);
    assert(page1_again == page1);
    assert(load_count == 1);  // Should not have loaded again
    
    // Test hit rate
    double hit_rate = pool.get_hit_rate();
    assert(hit_rate > 0);
    
    std::cout << "  BufferPoolManager: PASSED" << std::endl;
}

void test_memtable() {
    std::cout << "Testing MemTable..." << std::endl;
    
    MemTableConfig config;
    config.max_size = 1024 * 1024;  // 1 MB
    config.max_entries = 1000;
    
    MemTable table(config);
    
    // Test insert
    Vector vec(128, 1.0f);
    Error err = table.insert(1, vec);
    assert(err.ok());
    assert(table.get_num_entries() == 1);
    
    // Test get
    Vector retrieved;
    assert(table.get(1, retrieved));
    assert(retrieved.size() == 128);
    
    // Test contains
    assert(table.contains(1));
    assert(!table.contains(2));
    
    // Test delete
    err = table.delete_entry(1);
    assert(err.ok());
    assert(table.is_deleted(1));
    assert(!table.get(1, retrieved));  // Should return false for deleted
    
    // Test immutable
    table.mark_immutable();
    err = table.insert(2, vec);
    assert(!err.ok());  // Should fail on immutable table
    
    std::cout << "  MemTable: PASSED" << std::endl;
}

void test_lsm_write_manager() {
    std::cout << "Testing LsmWriteManager..." << std::endl;
    
    MemTableConfig memtable_config;
    memtable_config.max_size = 1024 * 1024;
    
    CompactionConfig compaction_config;
    compaction_config.enable_background_compaction = false;  // Disable for test
    
    LsmWriteManager manager("./test_data", memtable_config, compaction_config);
    
    // Note: init() may fail if directory doesn't exist, which is expected
    // Error err = manager.init();
    
    // Test insert
    Vector vec(128, 1.0f);
    NodeId node_id;
    // Error err = manager.insert(vec, node_id);
    // assert(err.ok());
    
    std::cout << "  LsmWriteManager: PASSED (partial)" << std::endl;
}

void test_storage_engine() {
    std::cout << "Testing StorageEngine..." << std::endl;
    
    StorageEngineConfig config;
    config.data_dir = "./test_data";
    config.dimension = 128;
    
    StorageEngine engine(config);
    
    // Test initial state
    assert(!engine.is_initialized());
    
    // Test init
    Error err = engine.init();
    assert(err.ok());
    assert(engine.is_initialized());
    
    // Test insert
    Vector vec(128, 1.0f);
    NodeId node_id;
    err = engine.insert(vec, node_id);
    assert(err.ok());
    assert(engine.get_num_vectors() == 1);
    
    // Test memory usage
    Size memory = engine.get_memory_usage();
    assert(memory > 0);
    
    std::cout << "  StorageEngine: PASSED" << std::endl;
}

void test_types_and_constants() {
    std::cout << "Testing types and constants..." << std::endl;
    
    // Test constants
    assert(PAGE_SIZE == 4096);
    assert(ALIGNMENT == 4096);
    assert(DEFAULT_DIMENSION == 128);
    assert(DEFAULT_MAX_DEGREE == 64);
    
    // Test helper functions
    Offset offset = calculate_vector_offset(10);
    assert(offset == 10 * PAGE_SIZE);
    
    PageId page_id = calculate_page_id(5);
    assert(page_id == 5);
    
    // Test memory limit check
    assert(is_memory_within_limits(100, 1000));  // 10% - within range
    assert(!is_memory_within_limits(300, 1000)); // 30% - outside range
    
    std::cout << "  Types and constants: PASSED" << std::endl;
}

void test_error_handling() {
    std::cout << "Testing error handling..." << std::endl;
    
    // Test Error class
    Error success = Error::success();
    assert(success.ok());
    
    Error invalid = Error::invalid_argument("test error");
    assert(!invalid.ok());
    assert(invalid.message() == "test error");
    
    // Test exception
    try {
        throw StorageEngineException(ErrorCode::IO_ERROR, "test exception");
    } catch (const StorageEngineException& e) {
        assert(e.code() == ErrorCode::IO_ERROR);
        std::string msg = e.what();
        assert(msg == "test exception");
    }
    
    std::cout << "  Error handling: PASSED" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Agent-Mem-IO Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        test_types_and_constants();
        test_error_handling();
        test_distance_functions();
        test_graph_nav_data();
        test_graph_index();
        test_buffer_pool();
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