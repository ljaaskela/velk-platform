cmake_minimum_required(VERSION 3.14)

project(velk-ui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/Zc:preprocessor /wd5105)
endif()

# Place all DLLs and executables together
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/)

# =============================================================================
# velk_compile_options: project-wide compile + link flags for velk code.
# Attached PRIVATE to each velk target near the end of this file. NOT applied
# to vendored third-party (shaderc, glslang, SPIRV-Tools, freetype, harfbuzz,
# cgltf, etc.) which need their own assumptions about RTTI/exceptions.
#
# MSVC handling of /GR- /EHs-c- is left to velk core's own CMakeLists, which
# also strips the inherited /EHsc /GR from the directory scope to avoid D9025
# warnings. Extending that to every velk-ui module would require the same
# string(REPLACE) at the top level (touching third-party FetchContent
# subdirs), so on MSVC velk_compile_options stays minimal.
# =============================================================================
add_library(velk_compile_options INTERFACE)

if(NOT MSVC)
    target_compile_options(velk_compile_options INTERFACE
        -fno-rtti
        -fno-exceptions
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        $<$<CONFIG:Release>:-flto=thin>
    )
    target_link_options(velk_compile_options INTERFACE
        $<$<CONFIG:Release>:-flto=thin>
    )
endif()

if(ANDROID)
    target_link_options(velk_compile_options INTERFACE
        "LINKER:--pack-dyn-relocs=android+relr"
        "LINKER:--icf=all"
        "LINKER:--exclude-libs,ALL"
    )
endif()

# =============================================================================
# Velk: build from source (sibling repo)
# =============================================================================
set(VELK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../velk" CACHE PATH "Path to velk source repository")

add_subdirectory("${VELK_SOURCE_DIR}/velk"                  "${CMAKE_BINARY_DIR}/velk")
add_subdirectory("${VELK_SOURCE_DIR}/velk/plugins/importer"  "${CMAKE_BINARY_DIR}/velk_importer")
add_subdirectory("${VELK_SOURCE_DIR}/velk/plugins/animator"  "${CMAKE_BINARY_DIR}/velk_animator")

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(velk-render)
add_subdirectory(velk-render/plugins/vk)
add_subdirectory(velk-render/plugins/rt)
add_subdirectory(velk-scene)
add_subdirectory(velk-ui)
add_subdirectory(plugins/text)
add_subdirectory(plugins/image)
add_subdirectory(plugins/gltf)
add_subdirectory(velk-runtime)
if(ANDROID)
    add_subdirectory(velk-runtime/plugins/android)
else()
    add_subdirectory(velk-runtime/plugins/glfw)
endif()
add_subdirectory(samples)

# =============================================================================
# Route plugin DLL outputs into a `plugins/` subdir alongside the exe, so
# the app root stays clean. Core velk.dll, executables, and third-party DLLs
# (shaderc_shared) stay at the exe directory. IPluginRegistry's
# `load_plugin("plugin:NAME")` resolver expects plugins under
# <exe_dir>/plugins/.
#
# Layout differs between single-config (Ninja, Makefiles, VS Open Folder /
# CMakeSettings.json) and multi-config (VS solution, Xcode) generators —
# match the CMAKE_RUNTIME_OUTPUT_DIRECTORY convention in each case so plugins
# are always a sibling subdir of the exe.
# =============================================================================
set(VELK_PLUGIN_TARGETS
    velk_importer velk_animator velk_tracy
    velk_render velk_vk velk_rt velk_scene velk_ui
    velk_text velk_image velk_gltf
    velk_runtime velk_runtime_glfw velk_runtime_android
)
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
foreach(plugin_target ${VELK_PLUGIN_TARGETS})
    if(NOT TARGET ${plugin_target})
        continue()
    endif()
    if(is_multi_config)
        foreach(config Release Debug RelWithDebInfo MinSizeRel)
            string(TOUPPER "${config}" config_upper)
            set_target_properties(${plugin_target} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/bin/${config}/plugins
                LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/bin/${config}/plugins
            )
        endforeach()
    else()
        set_target_properties(${plugin_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
        )
    endif()
endforeach()

# Attach velk_compile_options to every velk target (core + plugins).
set(VELK_ALL_TARGETS velk ${VELK_PLUGIN_TARGETS})
foreach(t ${VELK_ALL_TARGETS})
    if(TARGET ${t})
        target_link_libraries(${t} PRIVATE velk_compile_options)
    endif()
endforeach()
