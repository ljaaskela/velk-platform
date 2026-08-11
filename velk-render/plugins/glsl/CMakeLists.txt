find_package(Vulkan REQUIRED)

# shaderc: source built from the NDK via ndk-build on Android (static combined
# archive), shaderc_shared.dll from the Vulkan SDK on desktop (the SDK's
# shaderc_combined.lib is built with a newer MSVC than v142 ships and pulls in
# unresolved STL symbols when statically linked against an older toolchain).
#
# This plugin is the only thing in the tree that links shaderc.
if(ANDROID)
    set(NDK_SHADERC_DIR "${ANDROID_NDK}/sources/third_party/shaderc")
    set(NDK_SHADERC_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaderc-ndk-build")
    set(NDK_SHADERC_LIBS "${NDK_SHADERC_OUT}/obj/local/${ANDROID_ABI}")

    # NDK's Android.mk also defines a libshaderc_combined target that mri-script
    # llvm-ar all the component archives into one .a, but that step is broken on
    # Windows (the script's first line gets garbled). We sidestep it: ask
    # ndk-build for `shaderc` and link the component .a files individually.
    set(SHADERC_COMPONENT_LIBS
        "${NDK_SHADERC_LIBS}/libshaderc.a"
        "${NDK_SHADERC_LIBS}/libshaderc_util.a"
        "${NDK_SHADERC_LIBS}/libSPIRV-Tools-opt.a"
        "${NDK_SHADERC_LIBS}/libSPIRV-Tools.a"
        "${NDK_SHADERC_LIBS}/libglslang.a"
        "${NDK_SHADERC_LIBS}/libHLSL.a"
        "${NDK_SHADERC_LIBS}/libSPIRV.a"
        "${NDK_SHADERC_LIBS}/libOGLCompiler.a"
        "${NDK_SHADERC_LIBS}/libOSDependent.a"
    )

    if(CMAKE_HOST_WIN32)
        set(NDK_BUILD "${ANDROID_NDK}/ndk-build.cmd")
    else()
        set(NDK_BUILD "${ANDROID_NDK}/ndk-build")
    endif()

    include(ProcessorCount)
    ProcessorCount(NDK_BUILD_JOBS)
    if(NDK_BUILD_JOBS EQUAL 0)
        set(NDK_BUILD_JOBS 1)
    endif()

    add_custom_command(
        OUTPUT ${SHADERC_COMPONENT_LIBS}
        COMMAND "${NDK_BUILD}"
            "-j${NDK_BUILD_JOBS}"
            "NDK_PROJECT_PATH=${NDK_SHADERC_DIR}"
            "APP_BUILD_SCRIPT=${NDK_SHADERC_DIR}/Android.mk"
            "APP_STL=c++_shared"
            "APP_ABI=${ANDROID_ABI}"
            "APP_PLATFORM=android-${ANDROID_PLATFORM_LEVEL}"
            "NDK_LIBS_OUT=${NDK_SHADERC_OUT}/libs"
            "NDK_OUT=${NDK_SHADERC_OUT}/obj"
            shaderc shaderc_util
            glslang OSDependent OGLCompiler HLSL SPIRV
            SPIRV-Tools SPIRV-Tools-opt
        COMMENT "Building NDK shaderc (ndk-build -j${NDK_BUILD_JOBS}) for ${ANDROID_ABI}"
        VERBATIM
    )
    add_custom_target(shaderc_ndk_build DEPENDS ${SHADERC_COMPONENT_LIBS})

    add_library(shaderc INTERFACE)
    target_link_libraries(shaderc INTERFACE ${SHADERC_COMPONENT_LIBS})
    target_include_directories(shaderc INTERFACE "${NDK_SHADERC_DIR}/libshaderc/include")
else()
    find_library(SHADERC_LIB shaderc_shared PATHS "$ENV{VULKAN_SDK}/Lib" "${Vulkan_INCLUDE_DIRS}/../Lib")
    if(NOT SHADERC_LIB)
        message(FATAL_ERROR "shaderc_shared.lib not found. Ensure VULKAN_SDK is set.")
    endif()
    add_library(shaderc SHARED IMPORTED GLOBAL)
    set_target_properties(shaderc PROPERTIES
        IMPORTED_IMPLIB "${SHADERC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIRS}"
    )
endif()

add_library(velk_glsl SHARED
    src/glsl_plugin.h
    src/glsl_plugin.cpp
    src/glsl_shader_compiler.h
    src/glsl_shader_compiler.cpp
    include/velk-render/plugins/glsl/plugin.h
)

target_link_libraries(velk_glsl PRIVATE velk velk_render shaderc)

if(ANDROID)
    add_dependencies(velk_glsl shaderc_ndk_build)
endif()

target_include_directories(velk_glsl
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(velk_glsl PRIVATE VELK_EXPORTS)

if(NOT ANDROID)
    # shaderc_shared.dll must sit alongside velk_glsl.dll at runtime.
    #
    # Static linking against the SDK's shaderc_combined.lib does NOT work with
    # the v142 toolset: that archive is built against a newer MSVC STL and
    # pulls in __std_find_trivial_1 / __std_find_trivial_8, which 14.29 does
    # not provide. Retested 2026-08-11. Building shaderc from source the way
    # Android does is not an option here either, since the Vulkan SDK ships no
    # shaderc sources.
    get_filename_component(SHADERC_DIR "${SHADERC_LIB}" DIRECTORY)
    get_filename_component(VULKAN_BIN_DIR "${SHADERC_DIR}/../Bin" ABSOLUTE)
    add_custom_command(TARGET velk_glsl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VULKAN_BIN_DIR}/shaderc_shared.dll"
            "$<TARGET_FILE_DIR:velk_glsl>"
        COMMENT "Copying shaderc_shared.dll"
    )
endif()

