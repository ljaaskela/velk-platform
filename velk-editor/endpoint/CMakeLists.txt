add_library(velk_editor_endpoint SHARED
    src/endpoint_plugin.h
    src/endpoint_plugin.cpp
    include/velk-editor/endpoint/plugin.h
    include/velk-editor/endpoint/intf_editor_endpoint.h
)

target_link_libraries(velk_editor_endpoint PRIVATE velk velk_render velk_scene velk_ui)

target_include_directories(velk_editor_endpoint
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            $<TARGET_PROPERTY:velk_remote,INTERFACE_INCLUDE_DIRECTORIES>
            $<TARGET_PROPERTY:velk_importer,INTERFACE_INCLUDE_DIRECTORIES>
            ${CMAKE_SOURCE_DIR}/velk-runtime/include
            ${CMAKE_SOURCE_DIR}/plugins/image/include
)

target_compile_definitions(velk_editor_endpoint PRIVATE VELK_EXPORTS)
