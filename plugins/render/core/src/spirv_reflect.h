#ifndef VELK_UI_SPIRV_REFLECT_H
#define VELK_UI_SPIRV_REFLECT_H

#include <velk-ui/plugins/render/intf_render_backend.h>

#include <cstdint>

namespace velk_ui {

/**
 * @brief Extracts uniform metadata (name, location, type) from SPIR-V bytecode.
 *
 * Parses OpName, OpDecorate, OpVariable, and OpType* instructions to build
 * a list of active uniforms with their explicit layout locations and velk type UIDs.
 * Works identically for both GL and Vulkan SPIR-V.
 */
velk::vector<UniformInfo> reflect_spirv_uniforms(const uint32_t* spirv, size_t word_count);

} // namespace velk_ui

#endif // VELK_UI_SPIRV_REFLECT_H
