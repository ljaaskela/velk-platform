# =============================================================================
# FreeType: extract vendored zip and build as subdirectory
# =============================================================================
set(FREETYPE_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/freetype-src")
if(NOT EXISTS "${FREETYPE_EXTRACT_DIR}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype-2.13.3.zip"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
    )
    if(EXISTS "${CMAKE_BINARY_DIR}/_deps/freetype-2.13.3/CMakeLists.txt")
        file(REMOVE_RECURSE "${FREETYPE_EXTRACT_DIR}")
        file(RENAME "${CMAKE_BINARY_DIR}/_deps/freetype-2.13.3" "${FREETYPE_EXTRACT_DIR}")
    endif()
endif()

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory("${FREETYPE_EXTRACT_DIR}" "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)

# =============================================================================
# HarfBuzz: extract vendored zip and build as subdirectory
# =============================================================================
set(HARFBUZZ_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/harfbuzz-src")
if(NOT EXISTS "${HARFBUZZ_EXTRACT_DIR}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/harfbuzz-10.2.0.zip"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
    )
    if(EXISTS "${CMAKE_BINARY_DIR}/_deps/harfbuzz-10.2.0/CMakeLists.txt")
        file(REMOVE_RECURSE "${HARFBUZZ_EXTRACT_DIR}")
        file(RENAME "${CMAKE_BINARY_DIR}/_deps/harfbuzz-10.2.0" "${HARFBUZZ_EXTRACT_DIR}")
    endif()
endif()

set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
add_subdirectory("${HARFBUZZ_EXTRACT_DIR}" "${CMAKE_BINARY_DIR}/_deps/harfbuzz-build" EXCLUDE_FROM_ALL)

# =============================================================================
# Text plugin
# =============================================================================
add_library(velk_text SHARED
    src/text_plugin.h
    src/text_plugin.cpp
    src/font.h
    src/font.cpp
    src/font_atlas.h
    src/font_atlas.cpp
    src/visual/text_visual.h
    src/visual/text_visual.cpp
    src/embedded/inter_regular.h
    src/embedded/inter_regular.cpp
    include/velk-ui/plugins/text/api/font.h
    include/velk-ui/plugins/text/api/text_visual.h
    include/velk-ui/plugins/text/plugin.h
    include/velk-ui/plugins/text/intf_text_visual.h
)

target_link_libraries(velk_text PRIVATE velk velk_ui velk_render freetype harfbuzz)

target_include_directories(velk_text
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_text PRIVATE VELK_EXPORTS)
