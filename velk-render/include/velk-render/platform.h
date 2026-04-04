#ifndef VELK_RENDER_PLATFORM_H
#define VELK_RENDER_PLATFORM_H

#include <velk/common.h>

namespace velk {

// Backend class and plugin IDs, defined here so that velk_render (which selects
// the backend) and the backend plugins (which register against these IDs) share
// a single source of truth.

namespace ClassId {

inline constexpr Uid VkBackend{"f7a23c01-8e4d-4b19-a652-1d3f09b7e5c8"};

} // namespace ClassId

namespace PluginId {

inline constexpr Uid VkPlugin{"b91d4f6a-c583-47e0-9a1b-6e82d0f4a3b7"};

} // namespace PluginId

} // namespace velk

#endif // VELK_RENDER_PLATFORM_H
