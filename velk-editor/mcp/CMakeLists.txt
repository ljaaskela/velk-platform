add_executable(velk_editor_mcp
    src/main.cpp
    # The importer's JSON parser, for the protocol messages.
    ${VELK_SOURCE_DIR}/velk/plugins/importer/src/json_parser.cpp
)

target_link_libraries(velk_editor_mcp PRIVATE velk)
target_include_directories(velk_editor_mcp PRIVATE
    ${VELK_SOURCE_DIR}/velk/plugins/importer/src
    $<TARGET_PROPERTY:velk_remote,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:velk_editor_core,INTERFACE_INCLUDE_DIRECTORIES>
)
