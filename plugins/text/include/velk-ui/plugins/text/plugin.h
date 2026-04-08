#ifndef VELK_UI_TEXT_PLUGIN_H
#define VELK_UI_TEXT_PLUGIN_H

#include <velk/common.h>

namespace velk::ui {

namespace ClassId {

/** @brief FreeType + HarfBuzz font. Shapes text and rasterizes glyphs. */
inline constexpr Uid Font{"b31d28e5-2502-45fe-8b27-3997c882bfde"};

/** @brief IBuffer implementation for Font data. */
inline constexpr Uid FontGpuBuffer{"5367eff6-de78-4b76-8371-45562df5ba5c"};

/** @brief IMaterial that renders shaped text via analytic Bezier coverage. */
inline constexpr Uid TextMaterial{"e0d0f4f6-0c4b-4a8b-b7e4-7e2d6e1a0002"};

namespace Visual {

/** @brief Shaped text rendered as textured glyph quads from a glyph atlas. */
inline constexpr Uid Text{"51366e92-5d79-494b-9f5e-7185f8bc547b"};

} // namespace Visual

} // namespace ClassId

namespace PluginId {

inline constexpr Uid TextPlugin{"309d1c43-eeff-4f63-88ef-cd84297ef4c0"};

} // namespace PluginId

} // namespace velk::ui

#endif // VELK_UI_TEXT_PLUGIN_H
