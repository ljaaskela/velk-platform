cmake_minimum_required(VERSION 3.14)

project(velk-ui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/Zc:preprocessor /wd5105)
endif()

# Place all DLLs and executables together
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/)

# =============================================================================
# Velk: IMPORTED targets from sibling repo build
# =============================================================================
set(VELK_ROOT "${CMAKE_SOURCE_DIR}/../velk")
set(VELK_BUILD "${VELK_ROOT}/build")

# velk shared library (IMPORTED)
add_library(velk SHARED IMPORTED GLOBAL)
set_target_properties(velk PROPERTIES
    IMPORTED_LOCATION "${VELK_BUILD}/bin/Release/velk.dll"
    IMPORTED_IMPLIB   "${VELK_BUILD}/velk/Release/velk.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${VELK_ROOT}/velk/include;${VELK_BUILD}/velk/include"
)

# Importer plugin headers (for create_json_importer() and friends)
add_library(velk_importer_headers INTERFACE)
target_include_directories(velk_importer_headers INTERFACE
    "${VELK_ROOT}/velk/plugins/importer/include"
)
# Make velk_importer_headers available when linking velk
set_property(TARGET velk APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES velk_importer_headers
)

# =============================================================================
# GLFW: extract vendored zip and build as subdirectory
# =============================================================================
set(GLFW_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/glfw-src")
if(NOT EXISTS "${GLFW_EXTRACT_DIR}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf "${CMAKE_SOURCE_DIR}/third_party/glfw-3.4.zip"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
    )
    # The zip extracts to glfw-3.4/; rename to glfw-src
    if(EXISTS "${CMAKE_BINARY_DIR}/_deps/glfw-3.4/CMakeLists.txt")
        file(REMOVE_RECURSE "${GLFW_EXTRACT_DIR}")
        file(RENAME "${CMAKE_BINARY_DIR}/_deps/glfw-3.4" "${GLFW_EXTRACT_DIR}")
    endif()
endif()

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_EXTRACT_DIR}" "${CMAKE_BINARY_DIR}/_deps/glfw-build" EXCLUDE_FROM_ALL)

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(velk-ui)
add_subdirectory(plugins/render/gl)
add_subdirectory(app)
