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
inline constexpr Uid RenderTextureGroup{"e7e18400-dbd9-4ef4-bbbb-3b36f71fbf83"};

/** @brief Ray-traced shadow technique. Stub snippet; real body traces against the shared shape buffer. */
inline constexpr Uid RtShadow{"a1b54a78-cb8c-4c26-9d4e-413648cb280f"};

/** @brief Persistent IBuffer holding a program's per-draw data, cross-frame stable GPU address. */
inline constexpr Uid ProgramDataBuffer{"5362a373-42bf-48b1-9537-1229f44d008d"};

/** @brief Mesh container: an authored group of IMeshPrimitives plus aggregate bounds. */
inline constexpr Uid Mesh{"d82485b9-7d93-4196-ae98-f22f6131f7c8"};

/** @brief One geometry + material unit within a Mesh (glTF's "primitive"). */
inline constexpr Uid MeshPrimitive{"a523487f-60ff-4b9a-aab1-6e91de28d02f"};

/** @brief IBuffer storage for a MeshPrimitive's VBO / IBO bytes; may be shared across siblings. */
inline constexpr Uid MeshBuffer{"99f3245e-0115-4284-85ca-1c4a5ee93c97"};

/** @brief Factory for IMesh and IMeshPrimitive; hides the concrete impl classes. */
inline constexpr Uid MeshBuilder{"00332b65-d0f7-4b5f-bbed-764d8732be3e"};

/** @brief Per-frame GPU resource manager (texture / buffer / pipeline tracking + deferred destroy). */
inline constexpr Uid GpuResourceManager{"04f21ebb-d725-457b-97de-3a8d81262b9f"};

/** @brief Per-frame staging buffer (write/reserve interface for path-side data uploads). */
inline constexpr Uid FrameDataManager{"e957caf0-d5d3-4abb-925b-0d12f63dcb30"};

/** @brief Scene-wide registry of composed shader snippets (materials / shadow techs / intersects). */
inline constexpr Uid FrameSnippetRegistry{"6a013996-cdfa-4abf-a738-52eaf18d068f"};

/** @brief Per-frame render graph (Tier 1: ordered passes + barrier insertion). */
inline constexpr Uid RenderGraph{"cdeab6d1-97dc-480a-ae53-4eb0bc184baf"};

/** @brief Default `IRenderPass` implementation. Hive-pooled per-frame pass produced by render-path pipelines. */
inline constexpr Uid DefaultRenderPass{"ffc6e6c3-639a-461e-ad5c-7bc4ed902edf"};

namespace Path {

/** @brief Forward shading: classic raster pass over scene batches. Default fallback. */
inline constexpr Uid Forward{"3740b799-5c09-4bd2-9330-95ce71a24e33"};

/** @brief Deferred shading: G-buffer fill + compute lighting + composite blit. */
inline constexpr Uid Deferred{"2c7dfc96-724a-4eea-b10c-b9b99a131041"};

/** @brief Compute-shader path tracer. */
inline constexpr Uid Rt{"c5a2f31b-ca23-43c0-a19a-471c3c962942"};

} // namespace Path

namespace Pipeline {

/** @brief Default per-camera view pipeline. Composes the attached IRenderPath. */
inline constexpr Uid Camera{"2d3ad39e-aea2-4653-bafc-37ad5246a25b"};

} // namespace Pipeline

namespace Post {

/** @brief Linear post-process container. Composes attached IEffect children. */
inline constexpr Uid PostProcess{"63dde3c0-0db8-4671-8c20-e377928cd2a6"};

} // namespace Post

namespace Effect {

/** @brief ACES filmic tonemap effect. */
inline constexpr Uid Tonemap{"e58c8d7d-8339-4df4-85b9-0274a2256ab8"};

} // namespace Effect

} // namespace ClassId

namespace PluginId {

inline constexpr Uid RenderPlugin{"4dc6ab8e-3887-4def-a08e-59259ca39567"};

/** @brief Compute-shader path tracer (RtPath) sub-plugin. Loaded after velk_render. */
inline constexpr Uid RtPlugin{"b7c7c6a9-43a6-435c-b193-be467b1c9e85"};

} // namespace PluginId

} // namespace velk

#endif // VELK_RENDER_PLUGIN_H
