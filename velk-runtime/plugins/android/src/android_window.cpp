#include "android_window.h"

#include <velk/api/event.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <android/native_window.h>

namespace velk::impl {

void AndroidWindow::set_native_window(ANativeWindow* w)
{
    native_window_ = w;
    if (!w) {
        return;
    }
    int width = ANativeWindow_getWidth(w);
    int height = ANativeWindow_getHeight(w);
    VELK_LOG(I, "AndroidWindow: set_native_window %dx%d (surface_=%p)", width, height, surface_.get());
    write_state<IWindow>(this, [&](IWindow::State& s) {
        s.size = {static_cast<float>(width), static_cast<float>(height)};
    });
}

void AndroidWindow::notify_surface_created(ANativeWindow* w)
{
    set_native_window(w);
    // Publish the new native window + its dimensions into the surface state.
    // The renderer compares native_handle against its cached value and, on a
    // change (suspend/resume), drives backend->recreate_surface to rebuild the
    // platform surface + swapchain. Note: relies on the OS handing back a
    // distinct ANativeWindow* across destroy/recreate (it does in practice).
    if (w && surface_) {
        int width = ANativeWindow_getWidth(w);
        int height = ANativeWindow_getHeight(w);
        write_state<IWindowSurface>(surface_, [&](IWindowSurface::State& s) {
            s.native_handle = reinterpret_cast<uint64_t>(w);
            s.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        });
    }
    ::velk::invoke_event(get_interface(IInterface::UID), "on_surface_created");
}

void AndroidWindow::notify_surface_changed(int width, int height)
{
    VELK_LOG(I, "AndroidWindow: surface_changed %dx%d (surface_=%p)",
             width, height, surface_.get());
    write_state<IWindow>(this, [&](IWindow::State& s) {
        s.size = {static_cast<float>(width), static_cast<float>(height)};
    });
    // Mirror the size into the IWindowSurface state — the renderer keys off
    // this to call backend->resize_surface(), which recreates the swapchain
    // at the new dimensions. Without this the swapchain stays at the
    // original size and Android stretches the image on rotation.
    if (surface_) {
        write_state<IWindowSurface>(surface_, [&](IWindowSurface::State& s) {
            s.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        });
    }
    ::velk::size sz{static_cast<float>(width), static_cast<float>(height)};
    ::velk::invoke_event(get_interface(IInterface::UID), "on_resize", sz);
    ::velk::invoke_event(get_interface(IInterface::UID), "on_surface_changed", sz);
}

void AndroidWindow::notify_surface_destroyed()
{
    native_window_ = nullptr;
    ::velk::invoke_event(get_interface(IInterface::UID), "on_surface_destroyed");
}

void AndroidWindow::set_surface(IWindowSurface::Ptr surface)
{
    surface_ = std::move(surface);
}

void AndroidWindow::set_render_context(const IRenderContext::Ptr& ctx)
{
    render_ctx_ = ctx;
}

void AndroidWindow::set_input_dispatcher(ui::IInputDispatcher::Ptr dispatcher)
{
    input_ = std::move(dispatcher);
}

IWindowSurface::Ptr AndroidWindow::surface() const
{
    return surface_;
}

ui::IInputDispatcher& AndroidWindow::input() const
{
    return *input_;
}

IRenderContext::Ptr AndroidWindow::render_context() const
{
    return render_ctx_.lock();
}

bool AndroidWindow::should_close() const
{
    // NativeActivity drives the lifecycle; the window never asks to close
    // by itself. The host signals shutdown via the lifecycle events instead.
    return false;
}

} // namespace velk::impl
