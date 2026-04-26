add_library(velk_runtime SHARED
    src/runtime_plugin.h
    src/runtime_plugin.cpp
    src/application.h
    src/application.cpp
    include/velk-runtime/plugin.h
    include/velk-runtime/interface/intf_application.h
    include/velk-runtime/interface/intf_window.h
    include/velk-runtime/interface/intf_window_provider.h
    include/velk-runtime/api/application.h
    include/velk-runtime/api/window.h
)

target_link_libraries(velk_runtime PRIVATE velk velk_render velk_scene velk_ui velk_text)

target_include_directories(velk_runtime
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_runtime PRIVATE VELK_EXPORTS)
