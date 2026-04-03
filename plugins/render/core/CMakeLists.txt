find_package(Vulkan REQUIRED)

# shaderc: shared lib from the Vulkan SDK (avoids MSVC toolset version mismatch with static lib)
find_library(SHADERC_LIB shaderc_shared PATHS "$ENV{VULKAN_SDK}/Lib" "${Vulkan_INCLUDE_DIRS}/../Lib")
if(NOT SHADERC_LIB)
    message(FATAL_ERROR "shaderc_shared.lib not found. Ensure VULKAN_SDK is set.")
endif()

add_library(velk_render SHARED
    src/default_shaders.h
    src/render_context.h
    src/render_context.cpp
    src/render_plugin.h
    src/render_plugin.cpp
    src/renderer.h
    src/renderer.cpp
    src/shader_compiler.h
    src/shader_compiler.cpp
    src/surface.h
    src/intf_renderer_internal.h
    include/velk-ui/plugins/render/plugin.h
    include/velk-ui/plugins/render/intf_render_backend.h
    include/velk-ui/api/render_context.h
)

target_link_libraries(velk_render PRIVATE velk velk_ui ${SHADERC_LIB})

target_include_directories(velk_render PRIVATE ${Vulkan_INCLUDE_DIRS})

target_include_directories(velk_render
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

option(VELK_RENDER_DEBUG "Enable verbose render logging" OFF)

target_compile_definitions(velk_render PRIVATE VELK_EXPORTS)

if(VELK_RENDER_DEBUG)
    target_compile_definitions(velk_render PRIVATE VELK_RENDER_DEBUG)
endif()

# Copy shaderc_shared.dll to output directory
get_filename_component(SHADERC_DIR "${SHADERC_LIB}" DIRECTORY)
get_filename_component(VULKAN_BIN_DIR "${SHADERC_DIR}/../Bin" ABSOLUTE)
add_custom_command(TARGET velk_render POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VULKAN_BIN_DIR}/shaderc_shared.dll"
        "$<TARGET_FILE_DIR:velk_render>"
    COMMENT "Copying shaderc_shared.dll"
)
