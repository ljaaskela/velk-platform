#ifndef VELK_RUNTIME_APPLICATION_IMPL_H
#define VELK_RUNTIME_APPLICATION_IMPL_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-runtime/interface/intf_application.h>
#include <velk-runtime/interface/intf_window.h>
#include <velk-runtime/interface/intf_window_provider.h>
#include <velk-runtime/plugin.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/plugins/text/api/text_visual.h>

#include <chrono>

namespace velk::impl {

class Application : public ext::Object<Application, IApplication>
{
public:
    VELK_CLASS_UID(ClassId::Application, "Application");

    bool init(const ApplicationConfig& config) override;
    IWindow::Ptr create_window(const WindowConfig& config) override;
    IWindow::Ptr wrap_native_surface(void* native_handle) override;
    void add_view(const IObject::Ptr& window,
                  const ui::IElement::Ptr& camera,
                  const rect& viewport) override;
    bool poll() override;
    void update() override;
    ui::Frame prepare() override;
    void submit(ui::Frame frame) override;
    void present() override;
    IRenderContext::Ptr render_context() const override;
    ui::IRenderer::Ptr renderer() const override;
    void set_performance_overlay(const IObject::Ptr& window,
                                 const PerformanceOverlayConfig& config) override;
    void shutdown() override;

private:
    struct WindowSceneLink
    {
        IObject::Ptr window;
        shared_ptr<ui::IScene> scene;
        ScopedHandler resize_handler;
    };

    struct PerformanceOverlay
    {
        IObject::Ptr window;
        ui::Scene scene;
        ui::Element camera_element;
        ui::Element text_element;
        ui::TextVisual text_visual;
        int frame_count = 0;
        double cpu_time_accum = 0.0;
        double frame_time_accum = 0.0;
        std::chrono::steady_clock::time_point last_update;
    };

    void bind_scene_to_window(const IObject::Ptr& window, IWindow* iwin,
                              const shared_ptr<ui::IScene>& scene);

    /// Ensures render context + renderer exist after the first window is created.
    bool ensure_render_context();

    /// Updates performance overlay timings (called after each submit).
    void tick_overlays(double cpu_time, double frame_time,
                       std::chrono::steady_clock::time_point now);

    ApplicationConfig config_;
    IRenderContext::Ptr render_ctx_;
    ui::IRenderer::Ptr renderer_;
    IWindowProvider* window_provider_ = nullptr;
    IPlugin::Ptr platform_plugin_;
    vector<IObject::Ptr> windows_;
    vector<WindowSceneLink> scene_links_;
    vector<PerformanceOverlay> overlays_;
    std::chrono::steady_clock::time_point last_prepare_start_;
    std::chrono::steady_clock::time_point last_prepare_end_;
    std::chrono::steady_clock::time_point next_present_deadline_;
    std::chrono::nanoseconds target_frame_time_{0}; ///< Non-zero when at least one window uses UpdateRate::Targeted.
};

} // namespace velk::impl

#endif // VELK_RUNTIME_APPLICATION_IMPL_H
