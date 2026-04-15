#ifndef VELK_UI_PLUGIN_H
#define VELK_UI_PLUGIN_H

#include <velk/common.h>

namespace velk::ui {

namespace ClassId {

/** @brief UI element node. Holds position, size, transform, and z-index. */
inline constexpr Uid Element{"074fde67-68e0-43fb-b52d-265665f301ee"};

/** @brief Scene graph. Owns the element hierarchy, runs layout, and pushes changes to the renderer. */
inline constexpr Uid Scene{"03779f51-3fb0-45d7-9a9e-a25ef0a42dce"};

namespace Constraint {

/** @brief Lays out children along a single axis with optional spacing. */
inline constexpr Uid Stack{"677eae5c-efde-4130-97f6-5a09e9087629"};

/** @brief Clamps an element to a fixed width and/or height. */
inline constexpr Uid FixedSize{"f237dfd2-1a17-4078-9741-e0a7afd7b6a4"};

} // namespace Constraint

namespace Transform {

/** @brief Decomposed transform: translate, rotate (Z), scale. */
inline constexpr Uid Trs{"7c167a04-2d3e-41b9-8c42-6fd41be05794"};

/** @brief Raw 4x4 matrix transform. */
inline constexpr Uid Matrix{"44c08a4c-83f9-40f8-b900-b00d5a8f3e55"};

/** @brief Orients an element to face a target element. */
inline constexpr Uid LookAt{"aa7cb3fd-f93c-4929-855e-210661a019ad"};

/** @brief Positions and orients an element on a sphere around a target. */
inline constexpr Uid Orbit{"88ae2321-87dd-4c9f-8195-e50b5ad690dd"};

} // namespace Transform

namespace Visual {

/** @brief Solid color rectangle filling the element bounds. */
inline constexpr Uid Rect{"8fc18f28-03e8-4e6e-9fa9-274445d9ef4c"};

/** @brief Rounded rectangle with SDF corners. */
inline constexpr Uid RoundedRect{"327eb630-63f9-4144-ab05-e97d6099e920"};

/** @brief Displays a texture on the element bounds. */
inline constexpr Uid Texture{"84b11445-5f14-41f8-b949-d79aab19115d"};

} // namespace Visual

namespace Material {

/** @brief Built-in linear gradient material. */
inline constexpr Uid Gradient{"9eb060cd-6050-4878-b817-d58571bf3174"};

/** @brief Material that samples a texture with tint. Used by TextureVisual. */
inline constexpr Uid Texture{"5b68e32e-99a1-46e4-b029-de4bab50db06"};

} // namespace Material

namespace Render {

/** @brief Camera trait. Defines how the scene is observed (projection, zoom, scale). */
inline constexpr Uid Camera{"3cd4d525-fc81-4e27-a9c5-ac231036e474"};

/** @brief Caches an element's rendered subtree into a RenderTexture. */
inline constexpr Uid RenderCache{"d8a3aed1-cda4-4046-a69c-409ed7edc5c2"};

} // namespace Render

namespace Input {

/** @brief Scene-level input event coordinator. */
inline constexpr Uid Dispatcher{"4c51eabf-6f7a-455f-b34b-92e9bd3bab57"};

/** @brief Click gesture detection. Fires on_click on pointer down+up. */
inline constexpr Uid Click{"2906f6c0-055e-4b85-b2d9-2b58c89954a3"};

/** @brief Hover state tracking. Exposes hovered property and on_hover_changed event. */
inline constexpr Uid Hover{"0cf025ae-8a07-4b76-9559-1b4afb14c1f0"};

/** @brief Drag gesture tracking. Fires on_drag_start, on_drag_move, on_drag_end. */
inline constexpr Uid Drag{"e83b9c3e-36e5-4a45-b4e0-c72fcb8ed378"};

} // namespace Input

namespace Import {

/** @brief Type extension for dim (px/pct) values in the importer. */
inline constexpr Uid DimTypeExtension{"a52a1f22-8dd3-46bd-ba6c-07512a226e63"};

/** @brief Type extension for alignment enum values in the importer. */
inline constexpr Uid AlignTypeExtension{"0fce50cf-be04-430a-a617-5724ada76a30"};

/** @brief Type extension for Projection enum values in the importer. */
inline constexpr Uid ProjectionTypeExtension{"9dc1211c-70f1-41ea-a19f-099e2e7d66ef"};

/** @brief Type extension for VisualPhase enum values in the importer. */
inline constexpr Uid VisualPhaseTypeExtension{"8ebb28a3-c32a-4258-b09e-fa2c592d4382"};

} // namespace Import

} // namespace ClassId

namespace PluginId {

inline constexpr Uid VelkUiPlugin{"45c450a1-5f11-4869-8f72-3bafaeae0079"};

} // namespace PluginId

} // namespace velk::ui

#endif // VELK_UI_PLUGIN_H
