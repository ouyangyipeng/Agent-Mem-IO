# Agent-Mem-IO: Vector Retrieval Storage Engine for Agent Memory
# CMake Build Configuration
# 
# Copyright (c) 2026 Agent-Mem-IO Team
# Licensed under the MIT License

cmake_minimum_required(VERSION 3.20)

project(AgentMemIO
    VERSION 0.1.0
    DESCRIPTION "I/O Optimized Vector Retrieval System for Agent Memory"
    LANGUAGES CXX
)

# =============================================================================
# Project Options
# =============================================================================

option(ENABLE_TESTS "Enable unit tests" ON)
option(ENABLE_BENCHMARKS "Enable benchmark tests" ON)
option(ENABLE_PYTHON_BINDINGS "Enable Python bindings" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_IOURING "Enable io_uring support (requires Linux 5.1+)" ON)

# =============================================================================
# C++ Standard and Compiler Settings
# =============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Compiler Flags
# =============================================================================

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Common flags for both GCC and Clang
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror=return-type
        -Werror=uninitialized
        -fPIC
        -march=native
    )
    
    # Optimization flags for Release build
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    
    # Debug flags
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    
    # Sanitizer flags
    if(ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# =============================================================================
# Find Dependencies
# =============================================================================

# Threads
find_package(Threads REQUIRED)

# io_uring (Linux only)
if(ENABLE_IOURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(IOURING_INCLUDE_DIR "liburing.h" 
        PATHS /usr/include /usr/local/include
    )
    find_library(IOURING_LIBRARY "uring" 
        PATHS /usr/lib /usr/local/lib
    )
    
    if(IOURING_INCLUDE_DIR AND IOURING_LIBRARY)
        message(STATUS "Found io_uring: ${IOURING_LIBRARY}")
        set(IOURING_FOUND TRUE)
    else()
        message(WARNING "io_uring not found, falling back to POSIX AIO")
        set(IOURING_FOUND FALSE)
    endif()
else()
    set(IOURING_FOUND FALSE)
endif()

# Python for bindings (optional)
if(ENABLE_PYTHON_BINDINGS)
    find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
endif()

# =============================================================================
# Source Files
# =============================================================================

# Core library source files
set(CORE_SOURCES
    src/core/graph_index.cpp
    src/core/vamana_builder.cpp
    src/core/distance.cpp
    src/core/vector_dataset.cpp
    src/core/pq_encoder.cpp
    src/core/simd_distance.cpp
)

set(IO_SOURCES
    src/io/direct_io.cpp
    src/io/io_uring_engine.cpp
    src/io/file_manager.cpp
    src/io/disk_layout.cpp
)

set(BUFFER_SOURCES
    src/buffer/buffer_pool.cpp
    src/buffer/page.cpp
    src/buffer/eviction_policy.cpp
)

set(COMPACTION_SOURCES
    src/compaction/memtable.cpp
    src/compaction/sstable.cpp
    src/compaction/compaction_manager.cpp
    src/compaction/wal.cpp
)

set(ENGINE_SOURCES
    src/engine/storage_engine.cpp
    src/engine/query_processor.cpp
    src/engine/prefetcher.cpp
)

# All library sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${IO_SOURCES}
    ${BUFFER_SOURCES}
    ${COMPACTION_SOURCES}
    ${ENGINE_SOURCES}
)

# =============================================================================
# Library Target
# =============================================================================

add_library(agent_mem_io STATIC ${ALL_SOURCES})

target_include_directories(agent_mem_io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/io
        ${CMAKE_CURRENT_SOURCE_DIR}/src/buffer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/compaction
        ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
)

target_link_libraries(agent_mem_io
    PUBLIC
        Threads::Threads
)

# io_uring compile definition and linking
if(IOURING_FOUND)
    target_link_libraries(agent_mem_io PRIVATE ${IOURING_LIBRARY})
    target_compile_definitions(agent_mem_io PRIVATE USE_IOURING=1)
else()
    target_compile_definitions(agent_mem_io PRIVATE USE_IOURING=0)
endif()

# =============================================================================
# Executable Targets
# =============================================================================

# Main executable for testing
add_executable(agent_mem_io_cli
    src/main.cpp
)

target_link_libraries(agent_mem_io_cli
    PRIVATE agent_mem_io
)

# =============================================================================
# Test Targets
# =============================================================================

if(ENABLE_TESTS)
    enable_testing()
    
    # Test sources (single test file for now)
    set(TEST_SOURCES
        tests/test_main.cpp
    )
    
    add_executable(agent_mem_io_tests ${TEST_SOURCES})
    
    target_link_libraries(agent_mem_io_tests
        PRIVATE agent_mem_io
    )
    
    # Add tests
    add_test(NAME AgentMemIOTest COMMAND agent_mem_io_tests)
endif()

# =============================================================================
# Benchmark Targets
# =============================================================================

# Simple benchmark (always built)
add_executable(agent_mem_io_benchmark
    src/benchmark.cpp
)

target_link_libraries(agent_mem_io_benchmark
    PRIVATE agent_mem_io
)

# Full benchmark suite removed (benchmark.cpp is the main benchmark)

# =============================================================================
# Python Bindings
# =============================================================================

if(ENABLE_PYTHON_BINDINGS)
    add_subdirectory(src/bindings)
endif()

# =============================================================================
# Installation
# =============================================================================

install(TARGETS agent_mem_io
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY src/
    DESTINATION include/agent_mem_io
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# =============================================================================
# Print Configuration Summary
# =============================================================================

message(STATUS "")
message(STATUS "=== Agent-Mem-IO Configuration Summary ===")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Tests: ${ENABLE_TESTS}")
message(STATUS "Benchmarks: ${ENABLE_BENCHMARKS}")
message(STATUS "Python Bindings: ${ENABLE_PYTHON_BINDINGS}")
message(STATUS "Sanitizers: ${ENABLE_SANITIZERS}")
message(STATUS "io_uring: ${IOURING_FOUND}")
message(STATUS "")