#ifndef VELK_RUNTIME_INTF_WINDOW_H
#define VELK_RUNTIME_INTF_WINDOW_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-ui/interface/intf_input_dispatcher.h>

namespace velk {

/**
 * @brief A platform window or render target with input support.
 *
 * Wraps a native window (managed) or an externally provided platform surface
 * (framework-driven). Owns an ISurface for rendering, an IInputDispatcher for
 * routing input events to scene elements, and a weak reference to the
 * IRenderContext that produced its surface.
 *
 * Implementations live in platform plugins:
 *  - GLFW plugin (`velk_runtime_glfw`) for desktop
 *  - Android plugin (planned) for ANativeWindow wrapping
 *
 * Created indirectly via Application::create_window() or
 * Application::wrap_native_surface(), never instantiated directly.
 */
class IWindow : public Interface<IWindow>
{
public:
    VELK_INTERFACE(
        (RPROP, ::velk::size, size, {}),                       ///< Window size in pixels (read-only; updated by the platform).
        (EVT, on_resize, (::velk::size, new_size)),            ///< Fired when the window dimensions change.
        (EVT, on_surface_created),                             ///< Fired when a new platform surface is attached (framework-driven only).
        (EVT, on_surface_changed, (::velk::size, new_size)),   ///< Fired when an existing surface is resized (framework-driven only).
        (EVT, on_surface_destroyed)                            ///< Fired when the platform surface is being destroyed.
    )

    /** @brief Returns the render surface (swapchain target) for this window. */
    virtual IWindowSurface::Ptr surface() const = 0;

    /**
     * @brief Returns the input dispatcher bound to this window.
     *
     * The dispatcher routes pointer / scroll / key events through the bound
     * scene's element hierarchy. For managed windows the platform plugin feeds
     * events automatically; for wrapped surfaces the user feeds them by hand.
     */
    virtual ui::IInputDispatcher& input() const = 0;

    /**
     * @brief Returns the render context this window's surface belongs to.
     *
     * Implementations store this as a weak pointer and lock it on access, so
     * the window does not artificially extend the render context's lifetime.
     * Returns null if the render context has been destroyed.
     */
    virtual IRenderContext::Ptr render_context() const = 0;

    /**
     * @brief Returns true if the window has been requested to close.
     *
     * For managed (GLFW) windows this reflects @c glfwWindowShouldClose.
     * For wrapped (framework-driven) windows this always returns false — the
     * host framework controls the lifecycle, not velk.
     */
    virtual bool should_close() const = 0;
};

} // namespace velk

#endif // VELK_RUNTIME_INTF_WINDOW_H
