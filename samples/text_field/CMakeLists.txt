add_executable(velk_ui_text_field main.cpp)

target_link_libraries(velk_ui_text_field PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text)
target_include_directories(velk_ui_text_field PRIVATE
    $<TARGET_PROPERTY:velk_image,INTERFACE_INCLUDE_DIRECTORIES>
)

# The scene loads from app://, the executable's working directory.
add_custom_command(TARGET velk_ui_text_field POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/text_field.velk"
        "$<TARGET_FILE_DIR:velk_ui_text_field>/text_field.velk"
    COMMENT "Copying text_field.velk"
)
