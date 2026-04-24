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
# Subdirectories
# =============================================================================
add_subdirectory(velk-render)
add_subdirectory(velk-render/plugins/vk)
add_subdirectory(velk-ui)
add_subdirectory(plugins/text)
add_subdirectory(plugins/image)
add_subdirectory(plugins/gltf)
add_subdirectory(velk-runtime)
add_subdirectory(velk-runtime/plugins/glfw)
add_subdirectory(samples)
