# GoogleTest from velk's vendored archive.
set(GTEST_ARCHIVE ${VELK_SOURCE_DIR}/test/third_party/googletest-1.14.0.tar.gz)
set(GTEST_EXTRACT_DIR ${CMAKE_CURRENT_BINARY_DIR}/googletest)
if(NOT EXISTS ${GTEST_EXTRACT_DIR}/googletest-1.14.0/CMakeLists.txt)
    file(MAKE_DIRECTORY ${GTEST_EXTRACT_DIR})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${GTEST_ARCHIVE}
        WORKING_DIRECTORY ${GTEST_EXTRACT_DIR}
    )
endif()
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${GTEST_EXTRACT_DIR}/googletest-1.14.0 ${GTEST_EXTRACT_DIR}/build EXCLUDE_FROM_ALL)

add_executable(velk_editor_tests
    test_document.cpp
    test_core.cpp
    ../core/src/document.cpp
)

target_link_libraries(velk_editor_tests PRIVATE velk GTest::gtest_main)
target_include_directories(velk_editor_tests PRIVATE
    ../core/src
    $<TARGET_PROPERTY:velk_editor_core,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:velk_remote,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:velk_importer,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:velk_data,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(velk_editor_tests PRIVATE
    VELK_EDITOR_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixture"
)
# The core test launches velk_run with the endpoint, and loads these plugins.
add_dependencies(velk_editor_tests velk_run velk_editor_core velk_editor_endpoint velk_remote velk_importer velk_data)
