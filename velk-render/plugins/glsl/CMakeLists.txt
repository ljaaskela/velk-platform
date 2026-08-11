find_package(Vulkan REQUIRED)

# shaderc is linked statically on both platforms: built from NDK source via
# ndk-build on Android, and from the Vulkan SDK's shaderc_combined.lib on
# desktop. The plugin is therefore self-contained, and is the only thing in the
# tree that links shaderc at all.
#
# TOOLCHAIN FLOOR: shaderc_combined.lib is built against a recent MSVC STL and
# references __std_find_trivial_1 / __std_find_trivial_8. Toolsets that predate
# those fail to link it. Verified working with MSVC 14.44; verified failing
# with v142 / 14.29. Anyone hitting those unresolved symbols needs a newer
# toolset, not a source change.
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
    find_library(SHADERC_STATIC_LIB shaderc_combined PATHS "$ENV{VULKAN_SDK}/Lib" "${Vulkan_INCLUDE_DIRS}/../Lib")
    if(NOT SHADERC_STATIC_LIB)
        message(FATAL_ERROR "shaderc_combined.lib not found. Ensure VULKAN_SDK is set.")
    endif()
    add_library(shaderc STATIC IMPORTED GLOBAL)
    set_target_properties(shaderc PROPERTIES
        IMPORTED_LOCATION "${SHADERC_STATIC_LIB}"
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


