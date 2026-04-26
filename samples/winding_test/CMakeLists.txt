add_executable(velk_ui_winding_test
    main.cpp
)

target_link_libraries(velk_ui_winding_test PRIVATE velk velk_scene velk_ui velk_render velk_runtime)
