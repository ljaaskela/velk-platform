find_package(Vulkan REQUIRED)

# =============================================================================
# GLFW: extract vendored tarball and build as subdirectory
# =============================================================================
set(GLFW_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/glfw-src")
if(NOT EXISTS "${GLFW_EXTRACT_DIR}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw-3.4.tar.gz"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
    )
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
# GLFW runtime plugin
# =============================================================================
add_library(velk_runtime_glfw SHARED
    src/glfw_plugin.h
    src/glfw_plugin.cpp
    src/glfw_window.h
    src/glfw_window.cpp
    include/velk-runtime/plugins/glfw/plugin.h
)

target_link_libraries(velk_runtime_glfw PRIVATE velk velk_runtime velk_render velk_vk velk_scene velk_ui glfw)

target_include_directories(velk_runtime_glfw
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(velk_runtime_glfw PRIVATE VELK_EXPORTS VK_NO_PROTOTYPES)
