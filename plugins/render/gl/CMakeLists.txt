add_subdirectory(third_party/glad)

add_library(velk_gl SHARED
    src/gl_renderer.h
    src/gl_renderer.cpp
    src/gl_plugin.h
    src/gl_plugin.cpp
    include/velk-ui/plugins/gl/plugin.h
)

target_link_libraries(velk_gl PUBLIC glad2 PRIVATE velk velk_ui)

target_include_directories(velk_gl
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_gl PRIVATE VELK_EXPORTS)
