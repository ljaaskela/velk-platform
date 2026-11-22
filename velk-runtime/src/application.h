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
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-scene/interface/intf_renderer.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-ui/plugins/text/api/text_visual.h>

#include <chrono>

namespace velk::impl {

class Application : public ext::Object<Application, IApplication>
{
public:
    VELK_CLASS_UID(ClassId::Application, "Application");

    ~Application() override;
    bool init(const ApplicationConfig& config) override;
    IWindow::Ptr create_window(const WindowConfig& config) override;
    IWindow::Ptr wrap_native_surface(void* native_handle) override;
    void add_view(const IObject::Ptr& window,
                  const ::velk::IElement::Ptr& camera,
                  const rect& viewport) override;
    void remove_view(const IObject::Ptr& window, const ::velk::IElement::Ptr& camera) override;
    bool poll() override;
    void update() override;
    ::velk::Frame prepare() override;
    void submit(::velk::Frame frame) override;
    void present() override;
    IRenderContext::Ptr render_context() const override;
    ::velk::IRenderer::Ptr renderer() const override;
    void set_performance_overlay(const IObject::Ptr& window,
                                 const PerformanceOverlayConfig& config) override;
    void shutdown() override;

private:
    struct WindowSceneLink
    {
        IObject::Ptr window;
        shared_ptr<::velk::IScene> scene;
        ScopedHandler resize_handler;
        uint32_t views = 0;  ///< Views on the window showing the scene.
    };

    struct PerformanceOverlay
    {
        IObject::Ptr window;
        ::velk::Scene scene;
        ::velk::Element camera_element;
        ::velk::Element text_element;
        ui::TextVisual text_visual;
        int frame_count = 0;
        double cpu_time_accum = 0.0;
        double frame_time_accum = 0.0;
        std::chrono::steady_clock::time_point last_update;
    };

    void bind_scene_to_window(const IObject::Ptr& window, IWindow* iwin,
                              const shared_ptr<::velk::IScene>& scene);

    /// Ensures render context + renderer exist after the first window is created.
    bool ensure_render_context();
    /** @brief Loads the editor endpoint and listens, when a port is configured. */
    void start_editor_endpoint();
    void start_source_tracking();
    void set_resource_roots();
    bool load_config_plugins();

    /// Updates performance overlay timings (called after each submit).
    void tick_overlays(double cpu_time, double frame_time,
                       std::chrono::steady_clock::time_point now);

    /// Reads the first window's composite as RGBA8 into @p pixels and @p dims.
    bool read_first_window(vector<uint8_t>& pixels, uvec2& dims);
    bool read_window(const IObject::Ptr& window, vector<uint8_t>& pixels, uvec2& size) override;
    /// Saves RGBA8 @p pixels as a PNG at @p path.
    void save_capture(const vector<uint8_t>& pixels, uvec2 dims, const string& path);

    ApplicationConfig config_;
    /// Development aids, from VELK_AUTO_EXIT_FRAMES and VELK_AUTO_CAPTURE: poll()
    /// returns false after this many frames, first saving the image if a path is
    /// set. Lets any app run unattended and exit cleanly.
    long auto_exit_frames_ = 0;
    long presented_frames_ = 0;
    string auto_capture_path_;
    /// VELK_AUTO_CAPTURE_FRAMES: how many of the last frames before the exit are saved, each
    /// as the capture path with _1, _2 ... before its extension; kept in memory until the exit,
    /// so that saving does not hold up the frames between them.
    long auto_capture_frames_ = 1;
    struct AutoCapture
    {
        vector<uint8_t> pixels;
        uvec2 dims{};
    };
    vector<AutoCapture> auto_captures_;
    /// VELK_AUTO_GPU_STATS: logs GPU pass medians when VELK_AUTO_EXIT_FRAMES exits.
    bool auto_gpu_stats_ = false;
    struct AutoGpuPass
    {
        string label;
        vector<float> ms;
    };
    vector<AutoGpuPass> auto_gpu_samples_;
    IRenderContext::Ptr render_ctx_;
    ::velk::IRenderer::Ptr renderer_;
    IWindowProvider* window_provider_ = nullptr;
    IPlugin::Ptr platform_plugin_;
    IPlugin::Ptr editor_endpoint_; ///< Set when the editor endpoint is on.
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
