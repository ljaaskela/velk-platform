add_executable(velk_ui_inspect
    main.cpp
    scenes.cpp
    verify.cpp
)

target_link_libraries(velk_ui_inspect PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text velk_image)

# Where references are kept, and where each verification run writes its images and report,
# overwriting the last.
target_compile_definitions(velk_ui_inspect PRIVATE
    INSPECT_REFERENCE_DIR="${CMAKE_SOURCE_DIR}/reference_images/verify"
    INSPECT_RUN_DIR="${CMAKE_BINARY_DIR}/verify"
)
