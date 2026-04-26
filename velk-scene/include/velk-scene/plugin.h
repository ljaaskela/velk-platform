#ifndef VELK_SCENE_PLUGIN_H
#define VELK_SCENE_PLUGIN_H

#include <velk/common.h>

namespace velk {

namespace ClassId {

/** @brief UI element node. Holds position, size, transform, and z-index. */
inline constexpr Uid Element{"074fde67-68e0-43fb-b52d-265665f301ee"};

/** @brief Scene graph. Owns the element hierarchy, runs layout, and pushes changes to the renderer. */
inline constexpr Uid Scene{"03779f51-3fb0-45d7-9a9e-a25ef0a42dce"};

/** @brief Scene-wide BVH attachment; implements IBvh. Installed by the renderer on first frame. */
inline constexpr Uid SceneBvh{"d9678826-947d-4f92-beee-5746340fe072"};

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

/** @brief 3D axis-aligned box in the element's local frame. */
inline constexpr Uid Cube{"957908b0-41b3-4b0e-9b15-49f5c478af3c"};

/** @brief 3D sphere inscribed in the element's bounding box. */
inline constexpr Uid Sphere{"18072f38-60dd-436a-80b4-942bbca36b52"};

/** @brief Generic 3D mesh visual. Renders any IMesh assigned via IVisual3D::mesh. */
inline constexpr Uid Mesh{"db5043f9-ed9f-4032-b7c3-60c92a75f657"};

} // namespace Visual

namespace Render {

/** @brief Camera trait. Defines how the scene is observed (projection, zoom, scale). */
inline constexpr Uid Camera{"3cd4d525-fc81-4e27-a9c5-ac231036e474"};

/** @brief Light trait. Directional / point / spot source, with intrinsic colour and intensity. */
inline constexpr Uid Light{"6267f894-7953-45df-adb2-7eaaa5fe2def"};

} // namespace Render

} // namespace ClassId

namespace PluginId {

inline constexpr Uid ScenePlugin{"81d179c5-7e55-4623-98e8-b458c30147f9"};

} // namespace PluginId

} // namespace velk

#endif // VELK_SCENE_PLUGIN_H
