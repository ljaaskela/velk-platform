# Unit tests for velk-ui. GoogleTest comes from velk's vendored archive; the editor tests
# may already have added it.
if(NOT TARGET gtest_main)
    set(GTEST_ARCHIVE ${VELK_SOURCE_DIR}/test/third_party/googletest-1.14.0.tar.gz)
    set(GTEST_EXTRACT_DIR ${CMAKE_BINARY_DIR}/googletest)
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
endif()

add_executable(velk_ui_tests
    test_text_field.cpp
    test_scroll_view.cpp
    test_tree_view.cpp
    test_layout_visible.cpp
    test_list_view.cpp
)

target_link_libraries(velk_ui_tests PRIVATE velk velk_scene velk_ui velk_render velk_text GTest::gtest_main)
target_include_directories(velk_ui_tests PRIVATE $<TARGET_PROPERTY:velk_data,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(velk_ui_tests PRIVATE
    VELK_UI_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixture"
)
add_dependencies(velk_ui_tests velk_importer velk_data)
