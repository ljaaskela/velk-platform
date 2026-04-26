#ifndef VELK_RUNTIME_API_APPLICATION_H
#define VELK_RUNTIME_API_APPLICATION_H

#include <velk/api/object.h>
#include <velk/api/velk.h>

#include <velk-render/api/render_context.h>
#include <velk-runtime/api/window.h>
#include <velk-runtime/interface/intf_application.h>
#include <velk-runtime/plugin.h>
#include <velk-scene/api/element.h>

namespace velk {

/**
 * @brief Convenience wrapper around IApplication.
 *
 * Provides null-safe access to the application runtime. All methods return
 * safe defaults when the underlying object is null.
 *
 * Construct via velk::create_app() rather than directly.
 *
 * @par Example
 * @code
 * auto app = velk::create_app({});
 * auto window = app.create_window({.width = 1280, .height = 720});
 * auto scene = velk::create_scene("app://scenes/main.json");
 * app.add_view(window, scene.child_at(scene.root(), 0));
 * while (app.poll()) {
 *     app.update();
 *     app.present();
 * }
 * @endcode
 */
class Application : public Object
{
public:
    /** @brief Default-constructed Application wraps no object. */
    Application() = default;

    /** @brief Wraps an existing IObject pointer; rejected if it does not implement IApplication. */
    explicit Application(IObject::Ptr obj) : Object(check_object<IApplication>(obj)) {}

    /** @brief Wraps an existing IApplication pointer. */
    explicit Application(IApplication::Ptr a) : Object(as_object(a)) {}

    /** @brief Implicit conversion to IApplication::Ptr. */
    operator IApplication::Ptr() const { return as_ptr<IApplication>(); }

    /**
     * @brief Creates a new managed native window.
     * @param config Window dimensions, title, and pacing options.
     * @return The created Window, or an empty Window on failure.
     */
    Window create_window(const WindowConfig& config = {})
    {
        return Window(with<IApplication>([&](auto& a) { return a.create_window(config); }));
    }

    /**
     * @brief Wraps an externally-provided platform surface.
     *
     * For framework-driven cases where the host framework owns the native
     * window. The user feeds input events and resize notifications by hand.
     *
     * @param native_handle Platform-specific handle (HWND, ANativeWindow*, etc.).
     */
    Window wrap_native_surface(void* native_handle)
    {
        return Window(with<IApplication>([&](auto& a) { return a.wrap_native_surface(native_handle); }));
    }

    /**
     * @brief Binds a camera to a window's surface for rendering.
     * @param window   Target window.
     * @param camera   Camera element with an attached ICamera trait.
     * @param viewport Normalized viewport rect (0..1). Default = full surface.
     */
    void add_view(const IWindow::Ptr& window, const ::velk::IElement::Ptr& camera, const rect& viewport = {})
    {
        with<IApplication>(
            [&](auto& a) { a.add_view(interface_pointer_cast<::velk::IObject>(window), camera, viewport); });
    }

    /**
     * @brief Pumps platform events.
     * @return @c false when all windows have been closed (loop should exit).
     */
    bool poll()
    {
        return with<IApplication>([](auto& a) { return a.poll(); });
    }

    /** @brief Runs one velk update tick: scene layout, deferred updates, plugin hooks. */
    void update()
    {
        with<IApplication>([](auto& a) { a.update(); });
    }

    /**
     * @brief Prepares a frame on the calling thread.
     *
     * Builds draw commands for all registered views. The returned Frame handle
     * is then passed to submit().
     */
    ::velk::Frame prepare()
    {
        return with<IApplication>([](auto& a) { return a.prepare(); });
    }

    /**
     * @brief Submits a prepared frame to the GPU.
     *
     * Safe to call from any thread. For UpdateRate::Targeted windows, sleeps
     * after presenting to enforce the configured target framerate.
     */
    void submit(::velk::Frame frame)
    {
        with<IApplication>([&](auto& a) { a.submit(frame); });
    }

    /** @brief Convenience: prepare() + submit() in one synchronous call. */
    void present()
    {
        with<IApplication>([](auto& a) { a.present(); });
    }

    /** @brief Returns the render context owned by this application, wrapped for null-safety. */
    RenderContext render_context() const
    {
        return RenderContext(with<IApplication>([](auto& a) { return a.render_context(); }));
    }

    /** @brief Returns the default renderer owned by this application, or null if not yet created. */
    ::velk::IRenderer::Ptr renderer() const
    {
        return with<IApplication>([](auto& a) { return a.renderer(); });
    }

    /**
     * @brief Enables, disables, or reconfigures a performance overlay on a window.
     *
     * The overlay shows FPS / CPU time / frame time. Set @c config.enabled to
     * false to remove an existing overlay.
     */
    void set_performance_overlay(const Window& window,
                                 const PerformanceOverlayConfig& config = {})
    {
        with<IApplication>([&](auto& a) {
            a.set_performance_overlay(static_cast<IObject::Ptr>(window), config);
        });
    }
};

/**
 * @brief Creates and initializes an Application with the given config.
 *
 * Loads the runtime plugin, creates an IApplication implementation, and calls
 * init() on it. The init() in turn loads all standard plugins (UI, render,
 * text, image, importer, platform plugin) and prepares the runtime to accept
 * window creation calls.
 *
 * @param config Application-wide options.
 * @return The constructed Application, or an empty Application on failure.
 */
inline Application create_app(const ApplicationConfig& config = {})
{
    auto& v = instance();
    v.plugin_registry().load_plugin_from_path("velk_runtime.dll");
    auto app = v.create<IApplication>(ClassId::Application);
    return Application(app && app->init(config) ? std::move(app) : nullptr);
}

} // namespace velk

#endif // VELK_RUNTIME_API_APPLICATION_H
