find_package(Vulkan REQUIRED)

# shaderc: shared lib from the Vulkan SDK
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
    src/shader_compiler.h
    src/shader_compiler.cpp
    src/surface.h
    include/velk-render/plugin.h
    include/velk-render/platform.h
    include/velk-render/render_types.h
    include/velk-render/gpu_data.h
    include/velk-render/interface/intf_render_backend.h
    include/velk-render/interface/intf_render_context.h
    include/velk-render/interface/intf_surface.h
    include/velk-render/interface/intf_material.h
    include/velk-render/interface/intf_material_internal.h
    include/velk-render/interface/intf_texture_provider.h
    include/velk-render/detail/intf_renderer_internal.h
    include/velk-render/api/render_context.h
)

target_link_libraries(velk_render PRIVATE velk ${SHADERC_LIB})

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
