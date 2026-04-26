# =============================================================================
# glTF plugin: cgltf-based decoder for .gltf / .glb, MikkTSpace for tangents.
# =============================================================================

add_library(velk_gltf SHARED
    src/gltf_plugin.h
    src/gltf_plugin.cpp
    src/gltf_asset.h
    src/gltf_asset.cpp
    src/gltf_decoder.h
    src/gltf_decoder.cpp
    include/velk-ui/plugins/gltf/plugin.h
    include/velk-ui/plugins/gltf/interface/intf_gltf_asset.h
    # Vendored libraries, compiled into the plugin.
    third_party/mikktspace/mikktspace.h
    third_party/mikktspace/mikktspace.c
    third_party/cgltf/cgltf.h
)

target_link_libraries(velk_gltf PRIVATE velk velk_scene velk_ui velk_render velk_image)

target_include_directories(velk_gltf
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mikktspace
)

target_compile_definitions(velk_gltf PRIVATE VELK_EXPORTS)
