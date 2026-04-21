#ifndef VELK_RENDER_PLUGIN_H
#define VELK_RENDER_PLUGIN_H

#include <velk/common.h>

namespace velk {

namespace ClassId {

inline constexpr Uid RenderContext{"4a7c9e12-5d83-4b1f-a6e0-8f2d3c4b5a69"};
inline constexpr Uid Renderer{"8f4bdd2c-865b-4266-a1f1-abb921c9d60b"};
inline constexpr Uid WindowSurface{"ee9a45db-d4e3-44c4-bbee-19c244a5f32a"};
inline constexpr Uid ShaderMaterial{"04a29568-7677-42ee-9858-83b87196057f"};
inline constexpr Uid StandardMaterial{"4559e280-879d-4154-9565-d7346897588f"};

/** @brief Material property classes. Attached to StandardMaterial; see design-notes/material_properties.md. */
inline constexpr Uid BaseColorProperty{"28936f89-103a-4d63-81ec-f2f4abf781a2"};
inline constexpr Uid MetallicRoughnessProperty{"183e0b2e-86f2-4744-b28d-1a5584c532ea"};
inline constexpr Uid NormalProperty{"06a7f15d-0787-4b46-ba50-86c8bc2ef3a2"};
inline constexpr Uid OcclusionProperty{"9c20673d-48af-4431-9a88-08739644fd10"};
inline constexpr Uid EmissiveProperty{"72229e6c-15db-4cda-8d30-58e66e95c679"};
inline constexpr Uid SpecularProperty{"447cb08a-10b9-49f0-bcc5-18c57799608a"};

/** @brief Pipeline-state options attachable to any material. */
inline constexpr Uid MaterialOptions{"791cbf4f-7a8b-4343-b12a-a278fc46393e"};

inline constexpr Uid Shader{"1e766d8e-917b-419f-8262-14fbfb8fbb16"};
inline constexpr Uid RenderTexture{"ed3e9c55-9227-4c93-8c96-1759452e741f"};

/** @brief Ray-traced shadow technique. Stub snippet; real body traces against the shared shape buffer. */
inline constexpr Uid RtShadow{"a1b54a78-cb8c-4c26-9d4e-413648cb280f"};

/** @brief Persistent IBuffer holding a program's per-draw data, cross-frame stable GPU address. */
inline constexpr Uid ProgramDataBuffer{"5362a373-42bf-48b1-9537-1229f44d008d"};

/** @brief Geometry input to draw calls: VBO + IBO + layout descriptor. */
inline constexpr Uid Mesh{"d82485b9-7d93-4196-ae98-f22f6131f7c8"};

/** @brief IBuffer storage for a Mesh's VBO or IBO bytes. */
inline constexpr Uid MeshBuffer{"99f3245e-0115-4284-85ca-1c4a5ee93c97"};

/** @brief Factory for IMesh; create-and-fill helper that hides the impl class. */
inline constexpr Uid MeshBuilder{"00332b65-d0f7-4b5f-bbed-764d8732be3e"};

} // namespace ClassId

namespace PluginId {

inline constexpr Uid RenderPlugin{"4dc6ab8e-3887-4def-a08e-59259ca39567"};

} // namespace PluginId

} // namespace velk

#endif // VELK_RENDER_PLUGIN_H
