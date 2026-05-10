#ifndef VELK_RENDER_PLATFORM_H
#define VELK_RENDER_PLATFORM_H

#include <velk/common.h>

namespace velk {

// Backend class and plugin IDs, defined here so that velk_render (which selects
// the backend) and the backend plugins (which register against these IDs) share
// a single source of truth.

namespace ClassId {

inline constexpr Uid VkBackend{"f7a23c01-8e4d-4b19-a652-1d3f09b7e5c8"};
inline constexpr Uid VkCommandBuffer{"d64c732b-d642-4c4a-8baa-963af4e0b27e"};
inline constexpr Uid VkGpuBuffer{"ad5eb68e-c868-4803-a329-c9746df55efe"};
inline constexpr Uid VkGpuPipeline{"f801dbb1-c4bd-42a1-9b91-fe2c160bdd50"};
inline constexpr Uid VkGpuTexture{"dc12dbce-96b3-4246-8006-f1548ca86d5c"};
inline constexpr Uid VkRenderTexture{"b6c5c183-125f-44b5-9fe6-b9bd1ba394a9"};
inline constexpr Uid VkRenderTargetGroup{"c4290016-93ef-4f9e-b884-d7e01be4f458"};

} // namespace ClassId

namespace PluginId {

inline constexpr Uid VkPlugin{"b91d4f6a-c583-47e0-9a1b-6e82d0f4a3b7"};

} // namespace PluginId

} // namespace velk

#endif // VELK_RENDER_PLATFORM_H
