#ifndef VELK_RUNTIME_API_WINDOW_H
#define VELK_RUNTIME_API_WINDOW_H

#include <velk/api/event.h>
#include <velk/api/object.h>

#include <velk-render/api/render_context.h>
#include <velk-ui/api/input_dispatcher.h>
#include <velk-runtime/interface/intf_window.h>

namespace velk {

/**
 * @brief Convenience wrapper around IWindow.
 *
 * Provides null-safe access to window operations. All methods return safe
 * defaults (empty pointers, zero values) when the underlying object is null.
 *
 * @par Example
 * @code
 * auto window = app.create_window({.width = 1280, .height = 720});
 * auto surface = window.surface();
 * window.input()->pointer_event(ev);
 * @endcode
 */
class Window : public Object
{
public:
    /** @brief Default-constructed Window wraps no object. */
    Window() = default;

    /** @brief Wraps an existing IObject pointer; rejected if it does not implement IWindow. */
    explicit Window(IObject::Ptr obj) : Object(check_object<IWindow>(obj)) {}

    /** @brief Wraps an existing IWindow pointer. */
    explicit Window(IWindow::Ptr w) : Object(as_object(w)) {}

    /** @brief Implicit conversion to IWindow::Ptr. */
    operator IWindow::Ptr() const { return as_ptr<IWindow>(); }

    /** @brief Returns the current window size in pixels. */
    ::velk::size get_size() const { return read_state_value<IWindow>(&IWindow::State::size); }

    /** @brief Returns the render surface for this window, or null if not yet created. */
    IWindowSurface::Ptr surface() const
    {
        return with<IWindow>([](auto& w) { return w.surface(); });
    }

    /** @brief Returns the input dispatcher for this window, or null if the window is empty. */
    ui::IInputDispatcher* input() const
    {
        return with<IWindow>([](auto& w) -> ui::IInputDispatcher* { return &w.input(); });
    }

    /**
     * @brief Convenience: subscribes a handler to the window's pointer event stream.
     *
     * Wraps the input dispatcher's @c on_pointer_event in a ScopedHandler so the
     * subscription is RAII-managed.
     *
     * @param fn Callable invoked with each PointerEvent.
     * @return ScopedHandler that unsubscribes on destruction.
     */
    template <class F>
    ScopedHandler add_on_pointer_event(F&& fn)
    {
        return with<IWindow>(
            [&](auto& w) { return ScopedHandler(w.input().on_pointer_event(), std::move(fn)); });
    }

    /** @brief Returns the render context associated with this window, wrapped for null-safety. */
    RenderContext render_context() const
    {
        return RenderContext(with<IWindow>([](auto& w) { return w.render_context(); }));
    }

    /** @brief Resize event. Handler receives @c ::velk::size with the new dimensions. */
    Event on_resize() const
    {
        return with<IWindow>([](auto& w) { return w.on_resize(); });
    }

    /**
     * @brief Fired when a new platform surface is attached to this window.
     *
     * Only relevant for framework-driven windows where the host framework
     * creates and destroys surfaces dynamically (e.g. Android rotation).
     */
    Event on_surface_created() const
    {
        return with<IWindow>([](auto& w) { return w.on_surface_created(); });
    }

    /**
     * @brief Fired when the existing platform surface is resized.
     *
     * Handler receives a @c ::velk::size argument. Distinct from on_resize:
     * on_surface_changed reflects platform-driven surface dimension changes
     * (Android rotation, embedded host resize), while on_resize reflects
     * any window dimension change.
     */
    Event on_surface_changed() const
    {
        return with<IWindow>([](auto& w) { return w.on_surface_changed(); });
    }

    /**
     * @brief Fired when the platform surface is being destroyed.
     *
     * No arguments. Subscribers should release any GPU resources tied to the
     * surface before the handler returns.
     */
    Event on_surface_destroyed() const
    {
        return with<IWindow>([](auto& w) { return w.on_surface_destroyed(); });
    }

    /**
     * @brief Returns true if the window has been requested to close.
     *
     * For managed (GLFW) windows this reflects @c glfwWindowShouldClose.
     * Always false for wrapped (framework-driven) windows.
     */
    bool should_close() const
    {
        return with<IWindow>([](auto& w) { return w.should_close(); });
    }
};

} // namespace velk

#endif // VELK_RUNTIME_API_WINDOW_H
