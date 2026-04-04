find_package(Vulkan REQUIRED)

add_library(velk_vk SHARED
    src/vk_backend.h
    src/vk_backend.cpp
    src/vk_plugin.h
    src/vk_plugin.cpp
    src/volk_impl.c
    src/vma_impl.cpp
    include/velk-render/plugins/vk/plugin.h
)

target_link_libraries(velk_vk PRIVATE velk velk_render)

target_include_directories(velk_vk
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(velk_vk PRIVATE VELK_EXPORTS VK_NO_PROTOTYPES)
