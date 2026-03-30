# Python Bindings CMake Configuration
# This file configures the Python bindings for Agent-Mem-IO

# Find pybind11 if available
find_package(pybind11 CONFIG)

if(pybind11_FOUND)
    message(STATUS "Found pybind11: ${pybind11_VERSION}")
    
    # Create Python module
    pybind11_add_module(agent_mem_io_python
        python_bindings.cpp
    )
    
    target_link_libraries(agent_mem_io_python
        PRIVATE agent_mem_io
    )
    
    target_include_directories(agent_mem_io_python
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
else()
    message(STATUS "pybind11 not found, Python bindings will not be built")
endif()