#ifndef VELK_UI_VK_PLUGIN_H
#define VELK_UI_VK_PLUGIN_H

#include <velk-ui/plugins/render/platform.h>

namespace velk_ui {

/**
 * @brief Initialization parameters for the Vulkan backend.
 *
 * Passed via RenderConfig::backend_params.
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
