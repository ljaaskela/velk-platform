#include "application.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_plugin_registry.h>

#include <cstdio>
#include <thread>
#include <velk-render/api/render_context.h>
#include <velk-runtime/interface/intf_window.h>
#include <velk-ui/api/camera.h>
#include <velk-ui/api/element.h>
#include <velk-ui/api/renderer.h>
#include <velk-ui/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

namespace velk::impl {

bool Application::init(const ApplicationConfig& config)
{
    config_ = config;

    auto& v = instance();
    auto& reg = v.plugin_registry();

    // Load all standard plugins.
    reg.load_plugin_from_path("velk_ui.dll");
    reg.load_plugin_from_path("velk_render.dll");
    reg.load_plugin_from_path("velk_vk.dll");
    reg.load_plugin_from_path("velk_text.dll");
    reg.load_plugin_from_path("velk_image.dll");
    reg.load_plugin_from_path("velk_importer.dll");

    // Load the platform plugin (compile-time selection).
#if defined(__ANDROID__)
    reg.load_plugin_from_path("libvelk_runtime_android.so");
    platform_plugin_ = reg.find_plugin(PluginId::RuntimeAndroidPlugin);
#else
    reg.load_plugin_from_path("velk_runtime_glfw.dll");
    platform_plugin_ = reg.find_plugin(PluginId::RuntimeGlfwPlugin);
#endif
    if (!platform_plugin_) {
        VELK_LOG(E, "Failed to load platform plugin");
        return false;
    }

    window_provider_ = interface_cast<IWindowProvider>(platform_plugin_);
    if (!window_provider_) {
        VELK_LOG(E, "Platform plugin does not implement IWindowProvider");
        return false;
    }

    return true;
}

bool Application::ensure_render_context()
{
    if (render_ctx_) {
        return true;
    }

    VELK_PERF_SCOPE("app.ensure_render_context");

    // Create render context using the platform backend params populated
    // by the provider's first create_window/wrap_native_surface call.
    void* params = window_provider_->get_backend_params();
    RenderConfig render_config;
    render_config.backend = config_.backend;
    render_config.backend_params = params;
    render_ctx_ = create_render_context(render_config);
    if (!render_ctx_) {
        VELK_LOG(E, "Failed to create render context");
        return false;
    }

    renderer_ = ui::create_renderer(*render_ctx_);
    if (!renderer_) {
        VELK_LOG(E, "Failed to create renderer");
        return false;
    }
    return true;
}

IWindow::Ptr Application::create_window(const WindowConfig& config)
{
    if (!window_provider_) {
        return {};
    }

    VELK_PERF_SCOPE("app.create_window");

    bool first_window = !render_ctx_;
    auto win = window_provider_->create_window(config, render_ctx_);
    if (!win) {
        VELK_LOG(E, "Failed to create window");
        return {};
    }

    if (first_window) {
        if (!ensure_render_context()) {
            return {};
        }
        window_provider_->finalize_window(win, render_ctx_);
    }

    // Track the most restrictive target frame time across all Targeted windows.
    if (config.update_rate == UpdateRate::Targeted && config.target_fps > 0) {
        auto interval = std::chrono::nanoseconds(1'000'000'000LL / config.target_fps);
        if (target_frame_time_ == std::chrono::nanoseconds::zero() || interval < target_frame_time_) {
            target_frame_time_ = interval;
        }
    }

    windows_.emplace_back(as_object(win));
    return win;
}

IWindow::Ptr Application::wrap_native_surface(void* native_handle)
{
    if (!window_provider_) {
        return {};
    }

    bool first_window = !render_ctx_;
    auto win = window_provider_->wrap_native_surface(native_handle, render_ctx_);
    if (!win) {
        VELK_LOG(E, "Failed to wrap native surface");
        return {};
    }

    if (first_window) {
        if (!ensure_render_context()) {
            return {};
        }
        window_provider_->finalize_window(win, render_ctx_);
    }

    windows_.emplace_back(as_object(win));
    return win;
}

void Application::add_view(const IObject::Ptr& window,
                           const ui::IElement::Ptr& camera,
                           const rect& viewport)
{
    if (!renderer_) {
        return;
    }
    auto* iwin = interface_cast<IWindow>(window);
    if (!iwin) {
        return;
    }
    renderer_->add_view(camera, iwin->surface(), viewport);

    // Derive the scene from the camera element and bind resize.
    auto* elem = interface_cast<ui::IElement>(camera);
    if (elem) {
        auto scene = elem->get_scene();
        if (scene) {
            bind_scene_to_window(window, iwin, scene);

            // Also bind the window's input dispatcher to this scene.
            iwin->input().set_scene(scene);
        }
    }
}

void Application::bind_scene_to_window(const IObject::Ptr& window,
                                       IWindow* iwin,
                                       const shared_ptr<ui::IScene>& scene)
{
    // Check if this window-scene pair is already linked.
    for (auto& link : scene_links_) {
        auto* existing = interface_cast<IWindow>(link.window);
        if (existing == iwin && link.scene == scene) {
            return;
        }
    }

    // Set initial scene geometry from window size.
    auto sz = read_state<IWindow>(iwin)->size;
    scene->set_geometry(aabb::from_size(sz));

    // Subscribe to on_resize to update scene geometry synchronously
    // inside the platform event loop (matching old GLFW callback timing).
    weak_ptr<ui::IScene> weak_scene(scene);
    ScopedHandler resize_handler(iwin->on_resize(), [weak_scene](const ::velk::size& new_size) {
        auto s = weak_scene.lock();
        if (s) {
            s->set_geometry(aabb::from_size(new_size));
        }
    });

    WindowSceneLink link;
    link.window = window;
    link.scene = scene;
    link.resize_handler = std::move(resize_handler);
    scene_links_.emplace_back(std::move(link));
}

bool Application::poll()
{
    if (!window_provider_) {
        return false;
    }
    return window_provider_->poll_events();
}

void Application::update()
{
    instance().update();
}

ui::Frame Application::prepare()
{
    if (!renderer_) {
        return {};
    }
    last_prepare_start_ = std::chrono::steady_clock::now();
    auto frame = renderer_->prepare();
    last_prepare_end_ = std::chrono::steady_clock::now();
    return frame;
}

void Application::submit(ui::Frame frame)
{
    if (!renderer_) {
        return;
    }
    renderer_->present(frame);
    auto frame_end = std::chrono::steady_clock::now();

    // Software pacing for UpdateRate::Targeted: sleep until the next frame deadline.
    if (target_frame_time_ > std::chrono::nanoseconds::zero()) {
        if (next_present_deadline_.time_since_epoch().count() == 0) {
            next_present_deadline_ = frame_end + target_frame_time_;
        } else {
            next_present_deadline_ += target_frame_time_;
            // If we fell badly behind, snap forward instead of trying to catch up.
            if (next_present_deadline_ < frame_end) {
                next_present_deadline_ = frame_end + target_frame_time_;
            }
            std::this_thread::sleep_until(next_present_deadline_);
            frame_end = std::chrono::steady_clock::now();
        }
    }

    double cpu_time = std::chrono::duration<double>(last_prepare_end_ - last_prepare_start_).count();
    double frame_time = std::chrono::duration<double>(frame_end - last_prepare_start_).count();
    tick_overlays(cpu_time, frame_time, frame_end);
}

void Application::present()
{
    submit(prepare());
}

void Application::tick_overlays(double cpu_time, double frame_time,
                                std::chrono::steady_clock::time_point now)
{
    for (auto& ov : overlays_) {
        ov.frame_count++;
        ov.cpu_time_accum += cpu_time;
        ov.frame_time_accum += frame_time;

        double elapsed = std::chrono::duration<double>(now - ov.last_update).count();
        if (elapsed >= 0.5) {
            double fps = static_cast<double>(ov.frame_count) / elapsed;
            double avg_cpu_ms = ov.cpu_time_accum / static_cast<double>(ov.frame_count) * 1000.0;
            double avg_frame_ms = ov.frame_time_accum / static_cast<double>(ov.frame_count) * 1000.0;
            char buf[96];
            std::snprintf(
                buf, sizeof(buf), "%.0f fps\ncpu %.3f ms\ngpu %.3f ms", fps, avg_cpu_ms, avg_frame_ms);
            ov.text_visual.set_text(string(buf));
            ov.frame_count = 0;
            ov.cpu_time_accum = 0.0;
            ov.frame_time_accum = 0.0;
            ov.last_update = now;
        }
    }
}

IRenderContext::Ptr Application::render_context() const
{
    return render_ctx_;
}

ui::IRenderer::Ptr Application::renderer() const
{
    return renderer_;
}

void Application::set_performance_overlay(const IObject::Ptr& window,
                                          const PerformanceOverlayConfig& config)
{
    // Find existing overlay for this window.
    auto it = overlays_.begin();
    for (; it != overlays_.end(); ++it) {
        if (it->window == window) {
            break;
        }
    }

    if (!config.enabled && it != overlays_.end()) {
        overlays_.erase(it);
        return;
    }

    auto* iwin = interface_cast<IWindow>(window);
    if (!iwin || !renderer_) {
        return;
    }

    // Create the overlay scene with a root element, an ortho camera child,
    // and a text element child holding the FPS text visual.
    auto scene = ui::create_scene();
    auto root = ui::create_element();
    scene.set_root(root);

    auto camera_elem = ui::create_element();
    scene.add(root, camera_elem);

    auto camera_trait = ui::trait::render::create_camera();
    camera_trait.set_projection(ui::Projection::Ortho);
    camera_elem.add_trait(camera_trait);

    auto text_elem = ui::create_element();
    scene.add(root, text_elem);

    auto text_sz = ui::trait::layout::create_fixed_size(ui::dim::px(112), ui::dim::px(64));
    text_elem.add_trait(text_sz);

    auto text_bg = ui::trait::visual::create_rect();
    text_bg.set_color({0.f, 0.f, 0.f, .4f});
    text_elem.add_trait(text_bg);

    auto text_visual = ui::trait::visual::create_text();
    text_visual.set_font(ui::get_default_font());
    text_visual.set_text("FPS: ...");
    text_visual.set_font_size(config.font_size);
    text_visual.set_color(config.text_color);
    text_visual.set_layout(ui::TextLayout::MultiLine);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "- fps\ncpu - ms\ngpu - ms");
    text_visual.set_text(string(buf));

    text_elem.add_trait(text_visual);

    // Set initial scene geometry from window size.
    auto win_size = read_state<IWindow>(iwin)->size;
    scene.set_geometry(aabb::from_size(win_size));

    // Add the overlay as a view on the window's surface.
    // Added last so it's drawn on top of any existing user views.
    renderer_->add_view(camera_elem, iwin->surface(), {0, 0, 1, 1});

    // Bind resize so the overlay scene tracks window size.
    bind_scene_to_window(window, iwin, static_cast<ui::IScene::Ptr>(scene));

    PerformanceOverlay ov;
    ov.window = window;
    ov.scene = std::move(scene);
    ov.camera_element = std::move(camera_elem);
    ov.text_element = std::move(text_elem);
    ov.text_visual = std::move(text_visual);
    ov.last_update = std::chrono::steady_clock::now();

    if (it != overlays_.end()) {
        *it = std::move(ov);
    } else {
        overlays_.emplace_back(std::move(ov));
    }
}

void Application::shutdown()
{
    overlays_.clear();
    scene_links_.clear();
    windows_.clear();
    if (renderer_) {
        renderer_->shutdown();
        renderer_ = nullptr;
    }
    render_ctx_ = nullptr;
    window_provider_ = nullptr;
    platform_plugin_ = nullptr;
}

} // namespace velk::impl
