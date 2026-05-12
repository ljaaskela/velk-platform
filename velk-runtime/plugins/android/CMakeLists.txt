find_package(Vulkan REQUIRED)

# android_native_app_glue.c contains a reference to android_main, which lives
# in the NativeActivity-loaded binary (the sample), not in this plugin. Each
# sample adds the glue source to its own CMakeLists; we only need the header
# path so the plugin can reference glue types (android_app, android_poll_source,
# APP_CMD_*).
set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(velk_runtime_android SHARED
    src/android_plugin.h
    src/android_plugin.cpp
    src/android_log_sink.h
    src/android_log_sink.cpp
    src/android_asset_protocol.h
    src/android_asset_protocol.cpp
    src/android_window.h
    src/android_window.cpp
    include/velk-runtime/plugins/android/plugin.h
    include/velk-runtime/plugins/android/native_activity_main.h
)

target_link_libraries(velk_runtime_android PRIVATE
    velk velk_runtime velk_render velk_vk velk_scene velk_ui
    android log
)

target_include_directories(velk_runtime_android
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        ${NATIVE_APP_GLUE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(velk_runtime_android PRIVATE VELK_EXPORTS VK_NO_PROTOTYPES)
