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
# Velk: build from source (sibling repo)
# =============================================================================
set(VELK_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../velk" CACHE PATH "Path to velk source repository")

add_subdirectory("${VELK_SOURCE_DIR}/velk"                  "${CMAKE_BINARY_DIR}/velk")
add_subdirectory("${VELK_SOURCE_DIR}/velk/plugins/importer"  "${CMAKE_BINARY_DIR}/velk_importer")
add_subdirectory("${VELK_SOURCE_DIR}/velk/plugins/animator"  "${CMAKE_BINARY_DIR}/velk_animator")

# =============================================================================
# GLFW: extract vendored zip and build as subdirectory
# =============================================================================
set(GLFW_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/glfw-src")
if(NOT EXISTS "${GLFW_EXTRACT_DIR}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_SOURCE_DIR}/third_party/glfw-3.4.tar.gz"
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
add_subdirectory(test)
add_subdirectory(velk-render)
add_subdirectory(velk-render/plugins/vk)
add_subdirectory(velk-ui)
add_subdirectory(plugins/text)
add_subdirectory(plugins/image)
add_subdirectory(app)
