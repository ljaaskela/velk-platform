find_package(Vulkan REQUIRED)

# shaderc: shared lib from the Vulkan SDK
find_library(SHADERC_LIB shaderc_shared PATHS "$ENV{VULKAN_SDK}/Lib" "${Vulkan_INCLUDE_DIRS}/../Lib")
if(NOT SHADERC_LIB)
    message(FATAL_ERROR "shaderc_shared.lib not found. Ensure VULKAN_SDK is set.")
endif()

add_library(velk_render SHARED
    src/render_context.h
    src/render_context.cpp
    src/render_plugin.h
    src/render_plugin.cpp
    src/shader.h
    src/shader_cache.h
    src/shader_cache.cpp
    src/shader_compiler.h
    src/shader_compiler.cpp
    src/shader_material.h
    src/shader_material.cpp
    src/standard_material.h
    src/standard_material.cpp
    src/program_data_buffer.h
    src/program_data_buffer.cpp
    src/rt_shadow.h
    src/rt_shadow.cpp
    src/spirv_material_reflect.h
    src/spirv_material_reflect.cpp
    src/surface.h
    include/velk-render/plugin.h
    include/velk-render/platform.h
    include/velk-render/render_types.h
    include/velk-render/gpu_data.h
    include/velk-render/gbuffer.h
    include/velk-render/interface/intf_buffer.h
    include/velk-render/interface/intf_render_backend.h
    include/velk-render/interface/intf_render_context.h
    include/velk-render/interface/intf_surface.h
    include/velk-render/interface/material/intf_material.h
    include/velk-render/interface/material/intf_material_internal.h
    include/velk-render/interface/material/intf_material_options.h
    include/velk-render/interface/material/intf_material_property.h
    include/velk-render/interface/material/intf_shader_material.h
    include/velk-render/interface/material/intf_standard_material.h
    include/velk-render/interface/intf_shader.h
    include/velk-render/api/material/material.h
    include/velk-render/api/material/standard_material.h
    include/velk-render/api/material/material_options.h
    include/velk-render/api/material/material_property.h
    src/material_property.h
    include/velk-render/interface/intf_window_surface.h
    include/velk-render/ext/material.h
    include/velk-render/ext/gpu_resource.h
    include/velk-render/interface/intf_gpu_resource.h
    include/velk-render/interface/intf_window_surface.h
    include/velk-render/interface/intf_image.h
    include/velk-render/detail/intf_renderer_internal.h
    include/velk-render/api/render_context.h
    include/velk-render/api/material/shader_material.h
    include/velk-render/interface/intf_analytic_shape.h
    include/velk-render/interface/intf_bvh.h
    include/velk-render/interface/intf_program_data_buffer.h
    include/velk-render/interface/intf_draw_data.h
    include/velk-render/interface/intf_raster_shader.h
    include/velk-render/interface/intf_shader_snippet.h
    include/velk-render/interface/intf_render_technique.h
    include/velk-render/interface/intf_shadow_technique.h
    include/velk-render/api/render_technique.h
    include/velk-render/api/shadow_technique.h
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
