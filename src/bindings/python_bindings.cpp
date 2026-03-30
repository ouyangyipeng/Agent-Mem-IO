/**
 * @file python_bindings.cpp
 * @brief Python bindings for Agent-Mem-IO storage engine
 * 
 * This file provides Python bindings using pybind11, allowing the
 * storage engine to be used from Python and LangChain.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "engine/storage_engine.h"
#include "common/types.h"

namespace py = pybind11;
using namespace agent_mem_io;

PYBIND11_MODULE(agent_mem_io, m) {
    m.doc() = "Agent-Mem-IO: Vector Retrieval Storage Engine for Agent Memory";
    
    // Version info
    m.attr("__version__") = "0.1.0";
    
    // Error codes
    py::enum_<ErrorCode>(m, "ErrorCode")
        .value("SUCCESS", ErrorCode::SUCCESS)
        .value("INVALID_ARGUMENT", ErrorCode::INVALID_ARGUMENT)
        .value("IO_ERROR", ErrorCode::IO_ERROR)
        .value("MEMORY_LIMIT_EXCEEDED", ErrorCode::MEMORY_LIMIT_EXCEEDED)
        .value("NODE_NOT_FOUND", ErrorCode::NODE_NOT_FOUND)
        .value("GRAPH_BUILD_FAILED", ErrorCode::GRAPH_BUILD_FAILED)
        .value("COMPACTION_FAILED", ErrorCode::COMPACTION_FAILED)
        .value("WAL_ERROR", ErrorCode::WAL_ERROR)
        .value("BUFFER_POOL_FULL", ErrorCode::BUFFER_POOL_FULL)
        .value("UNKNOWN_ERROR", ErrorCode::UNKNOWN_ERROR);
    
    // SearchResult
    py::class_<SearchResult>(m, "SearchResult")
        .def(py::init<>())
        .def(py::init<NodeId, Distance>())
        .def_readwrite("node_id", &SearchResult::node_id)
        .def_readwrite("distance", &SearchResult::distance)
        .def("__repr__", [](const SearchResult& r) {
            return "SearchResult(node_id=" + std::to_string(r.node_id) + 
                   ", distance=" + std::to_string(r.distance) + ")";
        });
    
    // QueryResult
    py::class_<QueryResult>(m, "QueryResult")
        .def(py::init<>())
        .def_readwrite("results", &QueryResult::results)
        .def_readwrite("latency_ms", &QueryResult::latency_ms)
        .def_readwrite("io_count", &QueryResult::io_count)
        .def_readwrite("cache_hit_rate", &QueryResult::cache_hit_rate);
    
    // StorageEngineConfig
    py::class_<StorageEngineConfig>(m, "StorageEngineConfig")
        .def(py::init<>())
        .def_readwrite("data_dir", &StorageEngineConfig::data_dir)
        .def_readwrite("dimension", &StorageEngineConfig::dimension)
        .def_readwrite("memory_limit", &StorageEngineConfig::memory_limit)
        .def_readwrite("enable_prefetch", &StorageEngineConfig::enable_prefetch)
        .def_readwrite("enable_metrics", &StorageEngineConfig::enable_metrics);
    
    // StorageEngine
    py::class_<StorageEngine>(m, "StorageEngine")
        .def(py::init<const StorageEngineConfig&>())
        .def("init", &StorageEngine::init)
        .def("shutdown", &StorageEngine::shutdown)
        .def("load_dataset", &StorageEngine::load_dataset)
        .def("build_index", &StorageEngine::build_index)
        .def("insert", [](StorageEngine& engine, const std::vector<float>& vec) {
            NodeId node_id;
            Error err = engine.insert(Vector(vec.begin(), vec.end()), node_id);
            return std::make_pair(node_id, err.ok());
        })
        .def("search", [](StorageEngine& engine, const std::vector<float>& query, Size k) {
            SearchResults results;
            Error err = engine.search(Vector(query.begin(), query.end()), k, results);
            return std::make_pair(results, err.ok());
        })
        .def("get_num_vectors", &StorageEngine::get_num_vectors)
        .def("get_memory_usage", &StorageEngine::get_memory_usage)
        .def("is_memory_within_limits", &StorageEngine::is_memory_within_limits)
        .def("save", &StorageEngine::save)
        .def("load", &StorageEngine::load)
        .def("is_initialized", &StorageEngine::is_initialized)
        .def("flush", &StorageEngine::flush);
    
    // Factory functions
    m.def("create_storage_engine", &create_storage_engine,
          "Create storage engine with default configuration",
          py::arg("data_dir") = "./data",
          py::arg("memory_limit") = 0);
    
    m.def("create_sift1m_engine", &create_sift1m_engine,
          "Create storage engine configured for SIFT1M dataset",
          py::arg("data_dir") = "./data");
    
    // Distance functions
    m.def("l2_distance", [](const std::vector<float>& v1, const std::vector<float>& v2) {
        return l2_distance(Vector(v1.begin(), v1.end()), Vector(v2.begin(), v2.end()));
    }, "Calculate L2 distance between two vectors");
    
    m.def("cosine_distance", [](const std::vector<float>& v1, const std::vector<float>& v2) {
        return cosine_distance(Vector(v1.begin(), v1.end()), Vector(v2.begin(), v2.end()));
    }, "Calculate cosine distance between two vectors");
    
    // Constants
    m.attr("PAGE_SIZE") = PAGE_SIZE;
    m.attr("DEFAULT_DIMENSION") = DEFAULT_DIMENSION;
    m.attr("MIN_RECALL_AT_10") = MIN_RECALL_AT_10;
    m.attr("MEMORY_LIMIT_RATIO_MIN") = MEMORY_LIMIT_RATIO_MIN;
    m.attr("MEMORY_LIMIT_RATIO_MAX") = MEMORY_LIMIT_RATIO_MAX;
}