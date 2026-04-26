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
