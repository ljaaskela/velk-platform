# =============================================================================
# velk-scene: scene model + renderer.
#
# Owns IScene / IElement / IVisual interfaces and the render paths
# (Forward / Deferred / RayTrace) that consume them. velk-ui depends on
# velk-scene for these primitives; velk-scene depends on velk-render
# for GPU plumbing.
#
# See design-notes/architecture_layers.md for the layering plan.
# =============================================================================

add_library(velk_scene SHARED
    # Plugin entry
    src/plugin.h
    src/plugin.cpp

    # Concrete scene-model implementations
    src/element.h
    src/element.cpp
    src/scene.h
    src/scene.cpp

    # Renderer + render paths
    src/renderer.h
    src/renderer.cpp
    src/view_renderer.h
    src/rasterizer.h
    src/rasterizer.cpp
    src/deferred_lighter.h
    src/deferred_lighter.cpp
    src/ray_tracer.h
    src/ray_tracer.cpp

    # Scene-side rendering plumbing
    src/scene_bvh.h
    src/scene_bvh.cpp
    src/scene_collector.h
    src/scene_collector.cpp
    src/batch_builder.h
    src/batch_builder.cpp
    src/frame_data_manager.h
    src/frame_data_manager.cpp
    src/frame_snippet_registry.h
    src/frame_snippet_registry.cpp
    src/gpu_resource_manager.h
    src/gpu_resource_manager.cpp
    src/env_helper.h
    src/env_helper.cpp
    src/default_ui_shaders.h

    # 3D visuals
    src/visual/cube_visual.h
    src/visual/cube_visual.cpp
    src/visual/sphere_visual.h
    src/visual/sphere_visual.cpp
    src/visual/mesh_visual.h
    src/visual/mesh_visual.cpp
    src/visual/primitive_shaders.h

    # Render-feeding traits
    src/camera.h
    src/camera.cpp
    src/trait/light.h

    # Transform traits
    src/transform/trs.h
    src/transform/trs.cpp
    src/transform/matrix.h
    src/transform/matrix.cpp
    src/transform/look_at.h
    src/transform/look_at.cpp
    src/transform/orbit.h
    src/transform/orbit.cpp

    # Public interfaces (scene model + renderer surface)
    include/velk-scene/plugin.h
    include/velk-scene/types.h
    include/velk-scene/instance_types.h
    include/velk-scene/api/element.h
    include/velk-scene/api/scene.h
    include/velk-scene/api/trait.h
    include/velk-scene/interface/intf_scene_plugin.h
    include/velk-scene/interface/intf_scene.h
    include/velk-scene/interface/intf_scene_observer.h
    include/velk-scene/interface/intf_element.h
    include/velk-scene/interface/intf_visual.h
    include/velk-scene/interface/intf_renderer.h
    include/velk-scene/interface/intf_trait.h
    include/velk-scene/interface/intf_environment.h
    include/velk-scene/interface/intf_render_to_texture.h
    include/velk-scene/ext/material_shaders.h

    # Transform trait public surface
    include/velk-scene/interface/intf_transform_trait.h
    include/velk-scene/interface/trait/intf_trs.h
    include/velk-scene/interface/trait/intf_matrix.h
    include/velk-scene/interface/trait/intf_look_at.h
    include/velk-scene/interface/trait/intf_orbit.h
    include/velk-scene/api/trait/trs.h
    include/velk-scene/api/trait/matrix.h
    include/velk-scene/api/trait/look_at.h
    include/velk-scene/api/trait/orbit.h

    # Render-feeding traits public surface
    include/velk-scene/api/render_trait.h
    include/velk-scene/api/camera.h
    include/velk-scene/api/trait/light.h

    # 3D visual public surface
    include/velk-scene/api/mesh.h
    include/velk-scene/api/visual/visual.h
    include/velk-scene/api/visual/cube.h
    include/velk-scene/api/visual/sphere.h
    include/velk-scene/api/visual/mesh.h
    include/velk-scene/ext/trait.h
)

target_link_libraries(velk_scene PRIVATE velk velk_importer velk_render)

target_include_directories(velk_scene
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_scene PRIVATE VELK_EXPORTS)
