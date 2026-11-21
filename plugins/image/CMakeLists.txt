# =============================================================================
# libktx's reading side (KTX-Software 4.4.2): KTX1 and KTX2, zstd and zlib
# supercompression, Basis Universal transcoding, and ETC and ASTC decoding for
# devices that cannot sample them. The vendored zip holds only what reading
# needs; the ASTC library is built decompressor only.
# =============================================================================
set(KTX_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/ktx-src")
if(NOT EXISTS "${KTX_EXTRACT_DIR}/include/ktx.h")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/KTX-Software-4.4.2-read.zip"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
    )
    if(EXISTS "${CMAKE_BINARY_DIR}/_deps/KTX-Software-4.4.2/include/ktx.h")
        file(REMOVE_RECURSE "${KTX_EXTRACT_DIR}")
        file(RENAME "${CMAKE_BINARY_DIR}/_deps/KTX-Software-4.4.2" "${KTX_EXTRACT_DIR}")
    endif()
endif()

set(K "${KTX_EXTRACT_DIR}")
file(GLOB KTX_ASTC_SRC "${K}/external/astc-encoder/Source/astcenc_*.cpp")
add_library(velk_ktx_read STATIC
    ${K}/lib/astc_codec.cpp
    ${K}/lib/basis_transcode.cpp
    ${K}/lib/miniz_wrapper.cpp
    ${K}/lib/checkheader.c
    ${K}/lib/etcunpack.cxx
    ${K}/lib/filestream.c
    ${K}/lib/glformat_str.c
    ${K}/lib/hashlist.c
    ${K}/lib/info.c
    ${K}/lib/memstream.c
    ${K}/lib/strings.c
    ${K}/lib/swap.c
    ${K}/lib/texture.c
    ${K}/lib/texture1.c
    ${K}/lib/texture2.c
    ${K}/lib/vkformat_check.c
    ${K}/lib/vkformat_check_variant.c
    ${K}/lib/vkformat_str.c
    ${K}/lib/vkformat_typesize.c
    ${K}/external/basisu/transcoder/basisu_transcoder.cpp
    ${K}/external/basisu/zstd/zstd.c
    ${K}/external/dfdutils/createdfd.c
    ${K}/external/dfdutils/colourspaces.c
    ${K}/external/dfdutils/interpretdfd.c
    ${K}/external/dfdutils/printdfd.c
    ${K}/external/dfdutils/queries.c
    ${K}/external/dfdutils/vk2dfd.c
    ${K}/external/etcdec/etcdec.cxx
    ${KTX_ASTC_SRC}
)
target_include_directories(velk_ktx_read
    PUBLIC  ${K}/include
    PRIVATE ${K}/external/basisu/transcoder ${K}/external ${K}/external/basisu/zstd ${K}/utils ${K}/other_include
)
target_compile_definitions(velk_ktx_read
    PUBLIC  KHRONOS_STATIC KTX_FEATURE_KTX1 KTX_FEATURE_KTX2
    PRIVATE LIBKTX SUPPORT_SOFTWARE_ETC_UNPACK=1 KTX_OMIT_VULKAN=1
            BASISD_SUPPORT_FXT1=0 BASISD_SUPPORT_KTX2_ZSTD=0 BASISD_SUPPORT_KTX2=0
            ASTCENC_DECOMPRESS_ONLY=1
)
set_target_properties(velk_ktx_read PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Third-party code: its warnings are not ours to fix.
target_compile_options(velk_ktx_read PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/w> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(velk_ktx_read PRIVATE dl Threads::Threads)
endif()

# =============================================================================
# Image plugin: stb_image-based decoder for png/jpg/bmp/tga/hdr, libktx for ktx/ktx2.
# =============================================================================

add_library(velk_image SHARED
    src/image_plugin.h
    src/image_plugin.cpp
    src/image.h
    src/image.cpp
    src/image_decoder.h
    src/image_decoder.cpp
    src/ktx_decode.h
    src/ktx_decode.cpp
    src/image_encoder.h
    src/image_encoder.cpp
    src/image_material.h
    src/image_material.cpp
    src/image_visual.h
    src/image_visual.cpp
    src/environment.h
    src/environment.cpp
    src/env_decoder.h
    src/env_decoder.cpp
    src/env_material.h
    src/env_material.cpp
    include/velk-ui/plugins/image/plugin.h
    include/velk-ui/plugins/image/intf_image_material.h
    include/velk-ui/plugins/image/intf_image_visual.h
    include/velk-ui/plugins/image/intf_image_decoder.h
    include/velk-ui/plugins/image/api/image.h
    include/velk-ui/plugins/image/api/image_material.h
    include/velk-ui/plugins/image/api/image_visual.h
    include/velk-ui/plugins/image/api/environment.h
)

target_link_libraries(velk_image PRIVATE velk velk_scene velk_ui velk_render velk_ktx_read)

target_include_directories(velk_image
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        # libktx's lists of formats (vkformat_enum.h, vk_format.h), which its public header lacks.
        ${KTX_EXTRACT_DIR}/lib
)

target_compile_definitions(velk_image PRIVATE VELK_EXPORTS)
