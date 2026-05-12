find_package(Vulkan REQUIRED)

add_library(velk_vk SHARED
    src/vk_backend.h
    src/vk_backend.cpp
    src/vk_command_buffer.h
    src/vk_command_buffer.cpp
    src/vk_gpu_buffer.h
    src/vk_gpu_buffer.cpp
    src/vk_gpu_pipeline.h
    src/vk_gpu_pipeline.cpp
    src/vk_gpu_texture.h
    src/vk_gpu_texture.cpp
    src/vk_render_target_group.h
    src/vk_render_target_group.cpp
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

if(ANDROID)
    # NDK ships only core Vulkan headers; vma/ and Volk/ live in the desktop
    # Vulkan SDK install. Borrow them from there until we vendor them.
    if(NOT DEFINED ENV{VULKAN_SDK})
        message(FATAL_ERROR "VULKAN_SDK env var required for vma/volk headers on Android cross-compile.")
    endif()
    target_include_directories(velk_vk PRIVATE "$ENV{VULKAN_SDK}/Include")
endif()

target_compile_definitions(velk_vk PRIVATE VELK_EXPORTS VK_NO_PROTOTYPES)

if(VELK_RENDER_DEBUG)
    target_compile_definitions(velk_vk PRIVATE VELK_RENDER_DEBUG)
endif()
