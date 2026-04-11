#ifndef VELK_RUNTIME_INTF_APPLICATION_H
#define VELK_RUNTIME_INTF_APPLICATION_H

#include <velk/interface/intf_interface.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_types.h>
#include <velk-runtime/interface/intf_window.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_renderer.h>

namespace velk {

/**
 * @brief Window creation parameters.
 *
 * Passed to IApplication::create_window() to describe the desired window.
 * All fields have sensible defaults for a typical desktop UI window.
 */
struct WindowConfig
{
    int width{1280};                            ///< Initial window width in pixels.
    int height{720};                            ///< Initial window height in pixels.
    string_view title{"velk"};                  ///< Window title shown in the title bar.
    bool resizable{true};                       ///< Whether the user can resize the window.
    UpdateRate update_rate{UpdateRate::VSync};  ///< Surface pacing mode (VSync, Unlimited, or Targeted).
    uint32_t target_fps{60};                    ///< Target framerate for UpdateRate::Targeted
};

/**
 * @brief Application initialization parameters.
 *
 * Passed to velk::create_app() and forwarded to IApplication::init().
 * Controls runtime-wide options like the render backend selection.
 */
struct ApplicationConfig
{
    RenderBackendType backend{RenderBackendType::Default}; ///< GPU backend selection (Default = platform best).
    string_view app_root;                                  ///< Override path for the @c app:// resource scheme.
    string_view assets_root;                               ///< Override path for the @c assets:// resource scheme.
};

/**
 * @brief Performance overlay configuration.
 *
 * Passed to IApplication::set_performance_overlay() to enable, disable, or
 * customize the FPS / CPU / frame time overlay drawn on a window.
 */
struct PerformanceOverlayConfig
{
    bool enabled{true};                          ///< Set to false to remove the overlay from the window.
    float font_size{14.f};                       ///< Text size in pixels.
    color text_color{1.f, 1.f, 0.f, 1.f};        ///< Foreground text color (RGBA, default yellow).
};

/**
 * @brief Application runtime interface.
 *
 * Owns the render context and default renderer, manages the set of platform
 * windows, and exposes the frame lifecycle (poll/update/prepare/submit).
 *
 * Two creation paths are supported:
 *  - **Managed (desktop)**: create_window() asks the platform plugin to create
 *    a native window owned by velk. poll() pumps the GLFW event loop.
 *  - **Framework-driven (Android/embedded)**: wrap_native_surface() wraps a
 *    surface created by the host framework. The framework owns the event loop
 *    and calls update()/present() from its own callbacks.
 *
 * Instances are created via velk::create_app() rather than directly. Once
 * constructed, the application loads all standard plugins, sets up the render
 * context, and is ready to accept create_window() / wrap_native_surface() calls.
 */
class IApplication : public Interface<IApplication>
{
public:
    /**
     * @brief Initializes the runtime.
     *
     * Loads the platform plugin (GLFW or platform-specific), the render plugin,
     * the UI plugin, and other standard plugins. Stores the config for later use.
     * Must be called once before any other method.
     *
     * @param config Application-wide configuration.
     * @return @c true on success, @c false if a required plugin failed to load.
     */
    virtual bool init(const ApplicationConfig& config) = 0;

    /**
     * @brief Creates a new managed native window.
     *
     * Asks the platform plugin to create a native window matching @p config.
     * On the first call, also creates the render context and default renderer
     * lazily, since both depend on having a real surface to bind to.
     *
     * @param config Window dimensions, title, and pacing options.
     * @return The created window, or null on failure.
     */
    virtual IWindow::Ptr create_window(const WindowConfig& config) = 0;

    /**
     * @brief Wraps an externally-provided platform surface.
     *
     * Used for framework-driven cases (Android @c ANativeWindow*, embedded
     * @c HWND, etc.) where the host framework owns the native window. Dimensions
     * are queried from the native handle. The user is responsible for feeding
     * input events to @c window.input() and triggering on_resize when the
     * platform surface changes size.
     *
     * The wrapped window has no GLFW backing — its lifecycle is controlled by
     * the host framework, not by velk. should_close() always returns false.
     *
     * @param native_handle Platform-specific handle (HWND on Win32, ANativeWindow* on Android, etc.).
     * @return The wrapped window, or null if the platform plugin doesn't support wrapping.
     */
    virtual IWindow::Ptr wrap_native_surface(void* native_handle) = 0;

    /**
     * @brief Binds a camera to a window's surface for rendering.
     *
     * Registers a view with the default renderer: the camera element will be
     * rendered onto the window's surface every frame. Multiple views can target
     * the same window with different viewports for split-screen layouts.
     *
     * @param window   Target window (must be an IWindow).
     * @param camera   Camera element with an attached ICamera trait.
     * @param viewport Normalized viewport rect (0..1). Default = full surface.
     */
    virtual void add_view(const IObject::Ptr& window,
                          const ui::IElement::Ptr& camera,
                          const rect& viewport = {}) = 0;

    /**
     * @brief Pumps platform events.
     *
     * Drives the platform event loop (GLFW pollEvents on desktop). Returns
     * false when all managed windows have been requested to close, signaling
     * the host loop to exit.
     *
     * Framework-driven applications typically don't call this — the framework
     * delivers events directly to window input dispatchers.
     *
     * @return @c true to keep running, @c false to exit the loop.
     */
    virtual bool poll() = 0;

    /**
     * @brief Runs one velk update tick.
     *
     * Calls @c velk::instance().update(), which advances scene layout,
     * deferred property updates, animations, and any plugin update hooks.
     */
    virtual void update() = 0;

    /**
     * @brief Prepares a frame on the calling thread.
     *
     * Builds draw commands for all registered views and writes them into a
     * frame slot. Safe to call from the main / UI thread. The returned Frame
     * handle is then passed to submit().
     *
     * @return Opaque frame handle.
     */
    virtual ui::Frame prepare() = 0;

    /**
     * @brief Submits a prepared frame to the GPU.
     *
     * Submits the previously prepared frame to the render backend and presents
     * it. Safe to call from any thread (typically the render thread in
     * framework-driven cases). For UpdateRate::Targeted windows, sleeps after
     * presenting to enforce the target framerate.
     *
     * @param frame Frame handle returned by prepare().
     */
    virtual void submit(ui::Frame frame) = 0;

    /**
     * @brief Convenience: prepare() + submit() in one synchronous call.
     *
     * Use in single-threaded loops. For threaded rendering, call prepare() on
     * the main thread and submit() on the render thread.
     */
    virtual void present() = 0;

    /** @brief Returns the render context owned by this application, or null if not yet created. */
    virtual IRenderContext::Ptr render_context() const = 0;

    /** @brief Returns the default renderer owned by this application, or null if not yet created. */
    virtual ui::IRenderer::Ptr renderer() const = 0;

    /**
     * @brief Enables, disables, or reconfigures a performance overlay on a window.
     *
     * The overlay is a separate scene with an ortho camera and a text element
     * showing FPS, CPU time, and frame time. It is rendered on top of any
     * existing views on the window's surface, in its own private scene that
     * does not pollute the user's scene.
     *
     * @param window Target window.
     * @param config Overlay options. Set @c enabled = false to remove an existing overlay.
     */
    virtual void set_performance_overlay(const IObject::Ptr& window,
                                         const PerformanceOverlayConfig& config) = 0;

    /**
     * @brief Releases all resources.
     *
     * Destroys overlays, scene links, windows, the renderer, and the render context,
     * in dependency order. Called automatically when the Application is destroyed.
     */
    virtual void shutdown() = 0;
};

} // namespace velk

#endif // VELK_RUNTIME_INTF_APPLICATION_H
