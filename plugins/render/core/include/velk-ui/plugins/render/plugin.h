#ifndef VELK_UI_RENDER_PLUGIN_H
#define VELK_UI_RENDER_PLUGIN_H

#include <velk/common.h>

namespace velk_ui {

namespace ClassId {

/** @brief Render context. Owns the backend, creates renderers and surfaces. */
inline constexpr velk::Uid RenderContext{"4a7c9e12-5d83-4b1f-a6e0-8f2d3c4b5a69"};

/** @brief Backend-agnostic renderer. Manages surfaces, scenes, and batch collection. */
inline constexpr velk::Uid Renderer{"8f4bdd2c-865b-4266-a1f1-abb921c9d60b"};

/** @brief Render surface implementation. */
inline constexpr velk::Uid Surface{"ee9a45db-d4e3-44c4-bbee-19c244a5f32a"};

} // namespace ClassId

namespace PluginId {

inline constexpr velk::Uid RenderPlugin{"4dc6ab8e-3887-4def-a08e-59259ca39567"};

} // namespace PluginId

} // namespace velk_ui

#endif // VELK_UI_RENDER_PLUGIN_H
