# =============================================================================
# Image plugin: stb_image-based decoder for png/jpg/bmp/tga/hdr.
# =============================================================================

add_library(velk_image SHARED
    src/image_plugin.h
    src/image_plugin.cpp
    src/image.h
    src/image.cpp
    src/image_decoder.h
    src/image_decoder.cpp
    src/image_material.h
    src/image_material.cpp
    src/image_visual.h
    src/image_visual.cpp
    include/velk-ui/plugins/image/plugin.h
    include/velk-ui/plugins/image/intf_image_material.h
    include/velk-ui/plugins/image/intf_image_visual.h
)

target_link_libraries(velk_image PRIVATE velk velk_ui velk_render)

target_include_directories(velk_image
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

target_compile_definitions(velk_image PRIVATE VELK_EXPORTS)
