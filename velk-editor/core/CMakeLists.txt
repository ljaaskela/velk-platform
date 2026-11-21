add_library(velk_editor_core SHARED
    src/core_plugin.h
    src/core_plugin.cpp
    src/editor_core.h
    src/editor_core.cpp
    include/velk-editor/core/plugin.h
    include/velk-editor/core/intf_editor_core.h
)

# No velk-ui dependency: the core also runs headless.
target_link_libraries(velk_editor_core PRIVATE velk)

target_include_directories(velk_editor_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<TARGET_PROPERTY:velk_remote,INTERFACE_INCLUDE_DIRECTORIES>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_editor_core PRIVATE VELK_EXPORTS)
