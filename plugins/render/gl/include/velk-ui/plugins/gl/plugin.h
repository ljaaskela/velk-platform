#ifndef VELK_UI_GL_PLUGIN_H
#define VELK_UI_GL_PLUGIN_H

#include <velk/common.h>

namespace velk_ui {

namespace ClassId {

/** @brief OpenGL 3.3 renderer. Instanced quads for rects, textured quads for text. */
inline constexpr velk::Uid GlRenderer{"2302c979-1531-4d0b-bab6-d1bac99f0a11"};

} // namespace ClassId

namespace PluginId {

inline constexpr velk::Uid GlPlugin{"e1e9e004-21cd-4cfa-b843-49b0eb358149"};

} // namespace PluginId

} // namespace velk_ui

#endif // VELK_UI_GL_PLUGIN_H
