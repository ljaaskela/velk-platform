#ifndef VELK_VK_PLUGIN_H
#define VELK_VK_PLUGIN_H

#include <velk-render/platform.h>

namespace velk::vk {

using CreateSurfaceFn = bool (*)(void* vk_instance, void* out_surface, void* user_data);

struct VulkanInitParams
{
    CreateSurfaceFn create_surface = nullptr;
    void* user_data = nullptr;
};

} // namespace velk::vk

#endif // VELK_VK_PLUGIN_H
