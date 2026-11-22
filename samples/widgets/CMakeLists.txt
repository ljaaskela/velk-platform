add_executable(velk_ui_widgets main.cpp)

target_link_libraries(velk_ui_widgets PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text)
target_include_directories(velk_ui_widgets PRIVATE
    $<TARGET_PROPERTY:velk_image,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:velk_data,INTERFACE_INCLUDE_DIRECTORIES>
)

# The scene loads from app://, the executable's working directory.
add_custom_command(TARGET velk_ui_widgets POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/widgets.velk"
        "$<TARGET_FILE_DIR:velk_ui_widgets>/widgets.velk"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/widgets_row.velk"
        "$<TARGET_FILE_DIR:velk_ui_widgets>/widgets_row.velk"
    COMMENT "Copying widgets.velk and widgets_row.velk"
)
