add_executable(velk_editor_mcp
    src/main.cpp
)

target_link_libraries(velk_editor_mcp PRIVATE velk)
target_include_directories(velk_editor_mcp PRIVATE
    $<TARGET_PROPERTY:velk_remote,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:velk_editor_core,INTERFACE_INCLUDE_DIRECTORIES>
)
