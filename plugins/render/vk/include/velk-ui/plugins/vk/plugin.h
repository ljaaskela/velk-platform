#ifndef VELK_UI_VK_PLUGIN_H
#define VELK_UI_VK_PLUGIN_H

#include <velk/common.h>

namespace velk_ui {

namespace ClassId {

/** @brief Vulkan 1.2 render backend (bindless). */
inline constexpr velk::Uid VkBackend{"f7a23c01-8e4d-4b19-a652-1d3f09b7e5c8"};

} // namespace ClassId

namespace PluginId {

inline constexpr velk::Uid VkPlugin{"b91d4f6a-c583-47e0-9a1b-6e82d0f4a3b7"};

} // namespace PluginId

/**
 * @brief Initialization parameters for the Vulkan backend.
 *
 * Passed via RenderConfig::backend_params when RenderBackendType::Vulkan is selected.
 * The backend creates its own VkInstance, VkDevice, and VkSurfaceKHR.
 */
/**
 * @brief Callback to create a VkSurfaceKHR from the application's windowing system.
 *
 * The backend calls this during init because GLFW state is only available
 * in the main executable (GLFW is linked statically).
 */
using CreateSurfaceFn = bool(*)(void* vk_instance, void* out_surface, void* user_data);

struct VulkanInitParams
{
    CreateSurfaceFn create_surface = nullptr;
    void* user_data = nullptr;
};

} // namespace velk_ui

#endif // VELK_UI_VK_PLUGIN_H
