add_executable(velk_ui_inspect
    main.cpp
)

target_link_libraries(velk_ui_inspect PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text)
