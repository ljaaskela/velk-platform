if(ANDROID)
    # NativeActivity loads this .so via android.app.lib_name. android_main.cpp
    # provides the entry; android_native_app_glue.c (vendored by the NDK) is
    # the glue that defines ANativeActivity_onCreate and the worker thread.
    set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(velk_ui_simple SHARED
        main.cpp
        android_main.cpp
        ${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c
    )

    target_include_directories(velk_ui_simple PRIVATE ${NATIVE_APP_GLUE_DIR})

    target_link_libraries(velk_ui_simple PRIVATE
        velk velk_scene velk_ui velk_render velk_runtime velk_text velk_gltf
        velk_runtime_android
        android log
    )

    # Force ld to keep the ANativeActivity_onCreate symbol — without this,
    # link-time optimization or --gc-sections may strip it since nothing
    # inside the .so itself references it (NativeActivity dlsym-queries it).
    target_link_options(velk_ui_simple PRIVATE
        "LINKER:-u,ANativeActivity_onCreate"
    )
else()
    add_executable(velk_ui_simple
        main.cpp
    )

    target_link_libraries(velk_ui_simple PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text velk_gltf)

    # Stage scenes + assets next to the executable so the app:// resolver
    # finds them relative to the binary dir.
    add_custom_command(TARGET velk_ui_simple POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:velk_ui_simple>/scenes"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/scenes/dashboard.json"
            "$<TARGET_FILE_DIR:velk_ui_simple>/scenes/dashboard.json"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:velk_ui_simple>/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:velk_ui_simple>/scenes/images"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/scenes/images/velk-icon.png"
            "$<TARGET_FILE_DIR:velk_ui_simple>/scenes/images/velk-icon.png"
        COMMENT "Copying simple sample scenes + assets"
    )
endif()
