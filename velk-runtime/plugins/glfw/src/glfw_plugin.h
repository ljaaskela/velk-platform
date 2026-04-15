#ifndef VELK_RUNTIME_GLFW_PLUGIN_IMPL_H
#define VELK_RUNTIME_GLFW_PLUGIN_IMPL_H

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <velk/ext/plugin.h>
#include <velk/vector.h>

#include <velk-render/plugins/vk/plugin.h>
#include <velk-runtime/interface/intf_window_provider.h>
#include <velk-runtime/plugin.h>

namespace velk::impl {

class GlfwWindow;

class GlfwPlugin final : public ::velk::ext::Plugin<GlfwPlugin, IWindowProvider>
{
public:
    VELK_PLUGIN_UID(PluginId::RuntimeGlfwPlugin);
    VELK_PLUGIN_NAME("velk-runtime-glfw");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;

    IWindow::Ptr create_window(const WindowConfig& config,
                               const IRenderContext::Ptr& ctx) override;
    IWindow::Ptr wrap_native_surface(void* native_handle,
                                     const IRenderContext::Ptr& ctx) override;
    void finalize_window(const IWindow::Ptr& window,
                         const IRenderContext::Ptr& ctx) override;
    bool poll_events() override;
    void* get_backend_params() override;

private:
    GlfwWindow* get_glfw_window(const IWindow::Ptr& w) const;

    vk::VulkanInitParams vk_params_;
    vector<IWindow::Ptr> windows_;
};

} // namespace velk::impl

VELK_PLUGIN(velk::impl::GlfwPlugin)

#endif // VELK_RUNTIME_GLFW_PLUGIN_IMPL_H
