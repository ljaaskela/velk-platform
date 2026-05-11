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
# Velk: build from source (sibling repo)
# =============================================================================
set(VELK_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../velk" CACHE PATH "Path to velk source repository")

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
add_subdirectory(velk-runtime/plugins/glfw)
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
    velk_runtime velk_runtime_glfw
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
