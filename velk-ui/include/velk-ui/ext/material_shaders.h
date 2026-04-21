#ifndef VELK_UI_EXT_MATERIAL_SHADERS_H
#define VELK_UI_EXT_MATERIAL_SHADERS_H

#include <velk-render/ext/element_vertex.h>

namespace velk::ui {

/**
 * @brief Alias for the shared element vertex shader.
 *
 * Backward-compatibility name kept so older references don't break
 * while the codebase migrates; prefer `::velk::ext::element_vertex_src`
 * directly in new code.
 */
inline constexpr string_view rect_material_vertex_src = ::velk::ext::element_vertex_src;

} // namespace velk::ui

#endif // VELK_UI_EXT_MATERIAL_SHADERS_H
