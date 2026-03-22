#ifndef VELK_UI_TEXT_PLUGIN_H
#define VELK_UI_TEXT_PLUGIN_H

#include <velk/common.h>

namespace velk_ui {

namespace ClassId {

/** @brief FreeType + HarfBuzz font. Shapes text and rasterizes glyphs. */
inline constexpr velk::Uid Font{"f4a1b2c3-d5e6-4f78-9a0b-c1d2e3f4a5b6"};

namespace Visual {

/** @brief Shaped text rendered as textured glyph quads from a glyph atlas. */
inline constexpr velk::Uid Text{"b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e"};

} // namespace Visual

} // namespace ClassId

namespace PluginId {

inline constexpr velk::Uid TextPlugin{"a8b9c0d1-e2f3-4a5b-6c7d-8e9f0a1b2c3d"};

} // namespace PluginId

} // namespace velk_ui

#endif // VELK_UI_TEXT_PLUGIN_H
