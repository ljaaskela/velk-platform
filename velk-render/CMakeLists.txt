find_package(Vulkan REQUIRED)

# shaderc lives in the velk_glsl plugin (velk-render/plugins/glsl), the only
# thing that links it.

add_library(velk_render SHARED
    src/render_context.h
    src/render_context.cpp
    src/render_plugin.h
    src/render_plugin.cpp

    # Shader compile + cache
    src/shader/shader.h
    src/shader/shader_cache.h
    src/shader/shader_cache.cpp
    src/shader/velk_glsl.h
    src/shader/velk_glsl.cpp
    src/shader/shader_manager.h
    src/shader/shader_manager.cpp
    src/pipeline/pipeline_manager.h
    src/pipeline/pipeline_manager.cpp

    # Material implementations
    src/material/shader_material.h
    src/material/shader_material.cpp
    src/material/standard_material.h
    src/material/standard_material.cpp
    src/material/material_property.h
    src/material/spirv_material_reflect.h
    src/material/spirv_material_reflect.cpp
    src/material/material_unpacker.h
    src/material/material_unpacker.cpp

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
    include/velk-render/ext/gpu_buffer.h
    src/resource/gpu_buffer.h
    src/resource/program_data_buffer.h
    src/resource/gpu_resource_manager.h
    src/resource/gpu_resource_manager.cpp
    src/resource/gpu_arena.h
    src/resource/gpu_arena.cpp
    src/resource/arena_buffer.h
    src/resource/arena_buffer.cpp
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
    include/velk-render/interface/intf_gpu_command_buffer.h
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
    include/velk-render/interface/intf_shader_compiler.h
    include/velk-render/interface/intf_shader_manager.h
    include/velk-render/interface/intf_pipeline_manager.h
    include/velk-render/api/material/material.h
    include/velk-render/api/material/standard_material.h
    include/velk-render/api/material/material_options.h
    include/velk-render/api/material/material_property.h
    include/velk-render/interface/intf_window_surface.h
    include/velk-render/ext/material.h
    include/velk-render/ext/gpu_resource.h
    include/velk-render/ext/render_state.h
    include/velk-render/interface/intf_render_state.h
    include/velk-render/interface/intf_gpu_resource.h
    include/velk-render/interface/intf_window_surface.h
    include/velk-render/interface/intf_image.h
    include/velk-render/detail/intf_renderer_internal.h
    include/velk-render/detail/intf_gpu_resource_manager_internal.h
    include/velk-render/api/render_context.h
    include/velk-render/api/cached_view_pass.h
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
    include/velk-render/interface/intf_gpu_arena.h
    include/velk-render/interface/intf_frame_data_manager.h
    include/velk-render/interface/intf_frame_snippet_registry.h
    include/velk-render/interface/intf_render_graph.h
    include/velk-render/interface/intf_render_pass.h
    include/velk-render/interface/intf_render_stage.h
    include/velk-render/interface/intf_post_process.h
    include/velk-render/interface/intf_effect.h
    include/velk-render/interface/intf_render_texture_group.h
    include/velk-render/interface/intf_tonemap.h
    include/velk-render/interface/intf_bloom.h
    src/frame/render_graph.h
    src/frame/render_graph.cpp
    include/velk-render/ext/default_render_pass.h
    src/frame/default_render_pass.cpp
    src/frame/frame_data_manager.h
    src/frame/frame_data_manager.cpp
    src/frame/frame_snippet_registry.h
    src/frame/frame_snippet_registry.cpp
    include/velk-render/frame/draw_call_emit.h
    include/velk-render/frame/raster_shaders.h
    include/velk-render/frame/compute_shaders.h
    include/velk-render/frame/render_view.h

    # Render-path interfaces (under interface/) + shared types
    # (render_path/) + ext:: CRTP helpers (ext/); concrete impls in src/path/
    include/velk-render/interface/intf_render_path.h
    include/velk-render/interface/intf_view_pipeline.h
    include/velk-render/interface/intf_view_entry.h
    src/render_path/view_entry.h
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
    src/path/material_pipeline.h
    src/path/post_process.h
    src/path/post_process.cpp
    src/path/tonemap.h
    src/path/tonemap.cpp
    src/path/bloom.h
    src/path/bloom.cpp
    src/path/camera_pipeline.h
    src/path/camera_pipeline.cpp
)

target_link_libraries(velk_render PRIVATE velk)

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


