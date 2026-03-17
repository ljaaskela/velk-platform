add_library(velk_ui SHARED
    src/plugin.h
    src/plugin.cpp
    include/velk-ui/interface/intf_element.h
    include/velk-ui/interface/intf_renderer.h
    include/velk-ui/element.h
    include/velk-ui/plugin.h
)

target_link_libraries(velk_ui PRIVATE velk)

target_include_directories(velk_ui
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_ui PRIVATE VELK_EXPORTS)
