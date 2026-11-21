add_executable(velk_editor
    src/main.cpp
)

target_link_libraries(velk_editor PRIVATE velk velk_scene velk_ui velk_render velk_runtime)

# The editor UI is store files, read through app:// next to the executable.
add_custom_command(TARGET velk_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        "$<TARGET_FILE_DIR:velk_editor>/ui"
    COMMENT "Copying editor UI files"
)
