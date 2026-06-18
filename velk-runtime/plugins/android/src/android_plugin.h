#ifndef VELK_RUNTIME_ANDROID_PLUGIN_IMPL_H
#define VELK_RUNTIME_ANDROID_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>
#include <velk/interface/intf_log.h>
#include <velk/interface/resource/intf_resource_protocol.h>

#include <velk-render/plugins/vk/plugin.h>
#include <velk-runtime/interface/intf_window_provider.h>
#include <velk-runtime/plugin.h>

struct AAssetManager;
struct ANativeWindow;
struct android_app;

namespace velk::impl {

class AndroidWindow;

/**
 * @brief Android runtime plugin: NativeActivity host backend.
 *
 * Phase 2 scaffolding. Slice 2.3 ships an empty stub that registers via
 * IWindowProvider; lifecycle wiring (ANativeWindow -> IWindow events,
 * AAssetManager-backed app:// resolver, __android_log_print log sink)
 * lands in Slice 2.5.
 */
class AndroidPlugin final : public ::velk::ext::Plugin<AndroidPlugin, IWindowProvider>
{
public:
    VELK_PLUGIN_UID(PluginId::RuntimeAndroidPlugin);
    VELK_PLUGIN_NAME("velk-runtime-android");
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

    /// Process-wide setters for platform context. Called by
    /// android_native_activity_run before plugins load; consumed in
    /// initialize() and in the lifecycle/create_window paths.
    static void set_asset_manager(AAssetManager* mgr);
    static AAssetManager* asset_manager();

    static void set_android_app(struct android_app* app);
    static struct android_app* android_app_handle();

    /// Called by the APP_CMD_* command handler. Maps surface lifecycle
    /// to the active AndroidWindow's notify_surface_*.
    static void on_init_window(::ANativeWindow* w);
    static void on_term_window();
    static void on_window_resized(int width, int height);

private:
    vk::VulkanInitParams vk_params_;
    ILogSink::Ptr log_sink_;
    IResourceProtocol::Ptr app_protocol_;
};

} // namespace velk::impl

VELK_PLUGIN(velk::impl::AndroidPlugin)

#endif // VELK_RUNTIME_ANDROID_PLUGIN_IMPL_H
