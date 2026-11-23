add_executable(velk_ui_inspect
    main.cpp
    scenes.cpp
    timings.cpp
    verify.cpp
    ../common/nature_trees.cpp
)

target_link_libraries(velk_ui_inspect PRIVATE velk velk_scene velk_ui velk_render velk_runtime velk_text velk_image)

# Where references and the history of GPU times are kept, where each verification run writes
# its images and report, overwriting the last, and the repositories whose commits a run notes.
target_compile_definitions(velk_ui_inspect PRIVATE
    INSPECT_REFERENCE_DIR="${CMAKE_SOURCE_DIR}/reference_images/verify"
    INSPECT_RUN_DIR="${CMAKE_BINARY_DIR}/verify"
    INSPECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    INSPECT_VELK_DIR="${VELK_SOURCE_DIR}"
)
