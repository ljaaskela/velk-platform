add_executable(velk_ui_scale
    main.cpp
)

target_link_libraries(velk_ui_scale PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text)
