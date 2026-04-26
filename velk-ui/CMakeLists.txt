add_library(velk_ui SHARED
    src/plugin.h
    src/plugin.cpp
    src/layout_solver.h
    src/layout_solver.cpp
    src/layout/stack.h
    src/layout/stack.cpp
    src/constraint/fixed_size.h
    src/constraint/fixed_size.cpp
    src/visual/rect_visual.h
    src/visual/rect_visual.cpp
    src/visual/rounded_rect_visual.h
    src/visual/rounded_rect_visual.cpp
    src/visual/texture_visual.h
    src/visual/texture_visual.cpp
    src/visual/texture_material.h
    src/visual/texture_material.cpp
    src/render_cache.h
    src/render_cache.cpp
    src/material/gradient_material.h
    src/material/gradient_material.cpp
    src/import/align_type_extension.h
    src/import/align_type_extension.cpp
    src/import/dim_type_extension.h
    src/import/dim_type_extension.cpp
    src/import/light_type_extension.h
    src/import/light_type_extension.cpp
    src/import/projection_type_extension.h
    src/import/projection_type_extension.cpp
    src/import/visual_phase_type_extension.h
    src/import/visual_phase_type_extension.cpp
    src/input/input_dispatcher.h
    src/input/input_dispatcher.cpp
    src/input/click.h
    src/input/click.cpp
    src/input/hover.h
    src/input/hover.cpp
    src/input/drag.h
    src/input/drag.cpp
    include/velk-ui/input_types.h
    include/velk-ui/interface/intf_input_trait.h
    include/velk-ui/interface/intf_input_dispatcher.h
    include/velk-ui/ext/input.h
    include/velk-ui/api/input_dispatcher.h
    include/velk-ui/api/input/click.h
    include/velk-ui/api/input/hover.h
    include/velk-ui/api/input/drag.h
    include/velk-ui/interface/trait/intf_click.h
    include/velk-ui/interface/trait/intf_hover.h
    include/velk-ui/interface/trait/intf_drag.h
    include/velk-ui/interface/intf_font.h
    include/velk-ui/interface/intf_layout_trait.h
    include/velk-ui/interface/trait/intf_stack.h
    include/velk-ui/interface/trait/intf_fixed_size.h
    include/velk-ui/ext/trait.h
    include/velk-ui/api/trait/fixed_size.h
    include/velk-ui/api/trait/stack.h
    include/velk-ui/api/material/gradient.h
    include/velk-ui/api/visual/rect.h
    include/velk-ui/api/visual/rounded_rect.h
    include/velk-ui/interface/intf_gradient.h
    include/velk-ui/plugin.h
)

target_link_libraries(velk_ui PRIVATE velk velk_importer velk_render velk_scene)

target_include_directories(velk_ui
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

option(VELK_LAYOUT_DEBUG "Enable verbose layout logging" OFF)
option(VELK_INPUT_DEBUG "Enable verbose input logging" OFF)

target_compile_definitions(velk_ui PRIVATE VELK_EXPORTS)

if(VELK_LAYOUT_DEBUG)
    target_compile_definitions(velk_ui PRIVATE VELK_LAYOUT_DEBUG)
endif()

if(VELK_INPUT_DEBUG)
    target_compile_definitions(velk_ui PRIVATE VELK_INPUT_DEBUG)
endif()
