#ifndef VELK_RENDER_PLUGIN_H
#define VELK_RENDER_PLUGIN_H

#include <velk/common.h>

namespace velk {

namespace ClassId {

inline constexpr Uid RenderContext{"4a7c9e12-5d83-4b1f-a6e0-8f2d3c4b5a69"};
inline constexpr Uid Renderer{"8f4bdd2c-865b-4266-a1f1-abb921c9d60b"};
inline constexpr Uid Surface{"ee9a45db-d4e3-44c4-bbee-19c244a5f32a"};
inline constexpr Uid ShaderMaterial{"04a29568-7677-42ee-9858-83b87196057f"};
inline constexpr Uid Shader{"1e766d8e-917b-419f-8262-14fbfb8fbb16"};

} // namespace ClassId

namespace PluginId {

inline constexpr Uid RenderPlugin{"4dc6ab8e-3887-4def-a08e-59259ca39567"};

} // namespace PluginId

} // namespace velk

#endif // VELK_RENDER_PLUGIN_H
