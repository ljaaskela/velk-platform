#ifndef VELK_RUNTIME_ANDROID_WINDOW_IMPL_H
#define VELK_RUNTIME_ANDROID_WINDOW_IMPL_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-runtime/interface/intf_window.h>
#include <velk-runtime/plugins/android/plugin.h>
#include <velk-ui/interface/intf_input_dispatcher.h>

struct ANativeWindow;

namespace velk::impl {

/// IWindow backed by an ANativeWindow* delivered by the NativeActivity host.
/// The host pumps surface lifecycle events into set_native_window() /
/// notify_resized() in response to APP_CMD_INIT_WINDOW / APP_CMD_TERM_WINDOW
/// / APP_CMD_WINDOW_RESIZED.
class AndroidWindow : public ext::Object<AndroidWindow, IWindow>
{
public:
    VELK_CLASS_UID(ClassId::AndroidWindow, "AndroidWindow");

    void set_native_window(ANativeWindow* w);
    ANativeWindow* native_window() const { return native_window_; }

    /// Host (android_main / glue) calls these in response to APP_CMD_*
    /// events. They fire the corresponding IWindow EVTs and update state.
    void notify_surface_created(ANativeWindow* w);
    void notify_surface_changed(int width, int height);
    void notify_surface_destroyed();

    void set_surface(IWindowSurface::Ptr surface);
    void set_render_context(const IRenderContext::Ptr& ctx);
    void set_input_dispatcher(ui::IInputDispatcher::Ptr dispatcher);

    void set_pending_update_rate(UpdateRate r) { pending_update_rate_ = r; }
    void set_pending_target_fps(int fps) { pending_target_fps_ = fps; }
    void set_pending_depth(DepthFormat d) { pending_depth_ = d; }
    void set_pending_color_format(SurfaceColorFormat f) { pending_color_format_ = f; }

    UpdateRate pending_update_rate() const { return pending_update_rate_; }
    int pending_target_fps() const { return pending_target_fps_; }
    DepthFormat pending_depth() const { return pending_depth_; }
    SurfaceColorFormat pending_color_format() const { return pending_color_format_; }

    IWindowSurface::Ptr surface() const override;
    ui::IInputDispatcher& input() const override;
    IRenderContext::Ptr render_context() const override;
    bool should_close() const override;

private:
    ANativeWindow* native_window_ = nullptr;
    IWindowSurface::Ptr surface_;
    IRenderContext::WeakPtr render_ctx_;
    ui::IInputDispatcher::Ptr input_;
    UpdateRate pending_update_rate_ = UpdateRate::VSync;
    int pending_target_fps_ = 60;
    DepthFormat pending_depth_ = DepthFormat::None;
    SurfaceColorFormat pending_color_format_ = SurfaceColorFormat::RGBA8_SRGB;
};

} // namespace velk::impl

#endif // VELK_RUNTIME_ANDROID_WINDOW_IMPL_H
