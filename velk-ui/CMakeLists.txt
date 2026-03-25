add_library(velk_ui SHARED
    src/plugin.h
    src/plugin.cpp
    src/element.h
    src/element.cpp
    src/scene.h
    src/scene.cpp
    src/layout_solver.h
    src/layout_solver.cpp
    src/layout/stack.h
    src/layout/stack.cpp
    src/constraint/fixed_size.h
    src/constraint/fixed_size.cpp
    src/visual/rect_visual.h
    src/visual/rect_visual.cpp
    src/import/dim_type_extension.h
    src/import/dim_type_extension.cpp
    include/velk-ui/interface/intf_element.h
    include/velk-ui/interface/intf_font.h
    include/velk-ui/interface/intf_renderer.h
    include/velk-ui/interface/intf_constraint.h
    include/velk-ui/interface/intf_scene.h
    include/velk-ui/interface/intf_scene_observer.h
    include/velk-ui/interface/constraint/intf_stack.h
    include/velk-ui/interface/constraint/intf_fixed_size.h
    include/velk-ui/interface/intf_visual.h
    include/velk-ui/interface/intf_texture_provider.h
    include/velk-ui/ext/visual.h
    include/velk-ui/api/constraint/fixed_size.h
    include/velk-ui/api/constraint/stack.h
    include/velk-ui/api/element.h
    include/velk-ui/api/scene.h
    include/velk-ui/api/trait.h
    include/velk-ui/api/visual/rect.h
    include/velk-ui/interface/intf_trait.h
    include/velk-ui/plugin.h
    include/velk-ui/types.h
)

target_link_libraries(velk_ui PRIVATE velk velk_importer)

target_include_directories(velk_ui
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_ui PRIVATE VELK_EXPORTS)
