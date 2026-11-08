add_library(velk_rt SHARED
    src/rt_plugin.h
    src/rt_plugin.cpp
    src/rt_path.h
    src/rt_path.cpp
    src/reference_path.h
    src/reference_path.cpp
    include/velk-render/plugins/rt/plugin.h
)

target_link_libraries(velk_rt PRIVATE velk velk_render)

target_include_directories(velk_rt
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_rt PRIVATE VELK_EXPORTS)
