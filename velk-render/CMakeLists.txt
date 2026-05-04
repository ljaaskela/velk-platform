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

    # Shader compile + cache
    src/shader/shader.h
    src/shader/shader_cache.h
    src/shader/shader_cache.cpp
    src/shader/shader_compiler.h
    src/shader/shader_compiler.cpp

    # Material implementations
    src/material/shader_material.h
    src/material/shader_material.cpp
    src/material/standard_material.h
    src/material/standard_material.cpp
    src/material/material_property.h
    src/material/spirv_material_reflect.h
    src/material/spirv_material_reflect.cpp

    # Mesh primitives
    src/mesh/mesh.h
    src/mesh/mesh.cpp
    src/mesh/mesh_buffer.h
    src/mesh/mesh_buffer.cpp
    src/mesh/mesh_builder.h
    src/mesh/mesh_builder.cpp
    src/mesh/primitive_meshes.cpp

    # Concrete IGpuResource implementations
    src/resource/render_texture.h
    src/resource/render_texture_group.h
    src/resource/program_data_buffer.h
    src/resource/program_data_buffer.cpp
    src/resource/gpu_resource_manager.h
    src/resource/gpu_resource_manager.cpp
    src/resource/surface.h

    # Shadow / reflection / etc. techniques
    src/technique/rt_shadow.h
    src/technique/rt_shadow.cpp

    # Public headers
    include/velk-render/debug/render_target_dump.h
    include/velk-render/blas.h
    include/velk-render/frustum.h
    include/velk-render/interface/intf_mesh.h
    include/velk-render/interface/intf_primitive_shape.h
    include/velk-render/plugin.h
    include/velk-render/platform.h
    include/velk-render/render_types.h
    include/velk-render/gpu_data.h
    include/velk-render/interface/intf_batch.h
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
    include/velk-render/interface/intf_window_surface.h
    include/velk-render/ext/material.h
    include/velk-render/ext/gpu_resource.h
    include/velk-render/interface/intf_gpu_resource.h
    include/velk-render/interface/intf_window_surface.h
    include/velk-render/interface/intf_image.h
    include/velk-render/detail/intf_renderer_internal.h
    include/velk-render/detail/intf_gpu_resource_manager_internal.h
    include/velk-render/api/render_context.h
    include/velk-render/api/material/shader_material.h
    include/velk-render/interface/intf_analytic_shape.h
    include/velk-render/interface/intf_bvh.h
    include/velk-render/interface/intf_program_data_buffer.h
    include/velk-render/interface/intf_draw_data.h
    include/velk-render/interface/intf_shader_source.h
    include/velk-render/interface/intf_render_technique.h
    include/velk-render/interface/intf_shadow_technique.h
    include/velk-render/api/render_technique.h
    include/velk-render/api/shadow_technique.h

    # Per-frame runtime interfaces (interfaces under interface/, shared
    # types stay in frame/, render-graph impl in src/frame/)
    include/velk-render/interface/intf_gpu_resource_manager.h
    include/velk-render/interface/intf_frame_data_manager.h
    include/velk-render/interface/intf_frame_snippet_registry.h
    include/velk-render/interface/intf_render_graph.h
    include/velk-render/interface/intf_render_stage.h
    include/velk-render/interface/intf_post_process.h
    include/velk-render/interface/intf_effect.h
    include/velk-render/interface/intf_render_texture_group.h
    src/frame/render_graph.h
    src/frame/render_graph.cpp
    src/frame/frame_data_manager.h
    src/frame/frame_data_manager.cpp
    src/frame/frame_snippet_registry.h
    src/frame/frame_snippet_registry.cpp
    include/velk-render/frame/render_pass.h
    include/velk-render/frame/draw_call_emit.h
    include/velk-render/frame/raster_shaders.h
    include/velk-render/frame/compute_shaders.h
    include/velk-render/frame/render_view.h

    # Render-path interfaces (under interface/) + shared types
    # (render_path/) + ext:: CRTP helpers (ext/); concrete impls in src/path/
    include/velk-render/interface/intf_render_path.h
    include/velk-render/interface/intf_view_pipeline.h
    include/velk-render/render_path/view_entry.h
    include/velk-render/render_path/frame_context.h
    include/velk-render/ext/render_path.h
    include/velk-render/ext/view_pipeline.h
    include/velk-render/ext/post_process.h
    include/velk-render/ext/effect.h
    src/path/forward_path.h
    src/path/forward_path.cpp
    src/path/deferred_path.h
    src/path/deferred_path.cpp
    src/path/deferred_gbuffer.h
    src/path/post_process.h
    src/path/post_process.cpp
    src/path/tonemap.h
    src/path/tonemap.cpp
    src/path/camera_pipeline.h
    src/path/camera_pipeline.cpp
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
