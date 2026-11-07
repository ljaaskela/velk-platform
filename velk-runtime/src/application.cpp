#include "application.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_plugin_registry.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <velk-render/api/render_context.h>
#include <velk-render/interface/intf_image.h>
#include <velk-ui/plugins/image/plugin.h>
#include <velk-render/detail/intf_renderer_internal.h>
#include <velk-runtime/interface/intf_window.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-ui/api/renderer.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-scene/interface/intf_element.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

namespace velk::impl {

namespace {

float half_to_float(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exponent = (h >> 10) & 0x1fu;
    const uint32_t mantissa = h & 0x3ffu;
    uint32_t bits = sign;
    if (exponent == 0x1fu) {
        bits |= 0x7f800000u | (mantissa << 13);
    } else if (exponent != 0) {
        bits |= ((exponent + 112u) << 23) | (mantissa << 13);
    } else if (mantissa != 0) {
        uint32_t m = mantissa;
        uint32_t e = 113u;
        while ((m & 0x400u) == 0) {
            m <<= 1;
            --e;
        }
        bits |= (e << 23) | ((m & 0x3ffu) << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

Application::~Application()
{
    overlays_.clear();
    scene_links_.clear();
    windows_.clear();
    renderer_.reset();
    platform_plugin_.reset();
    render_ctx_.reset();
}

bool Application::init(const ApplicationConfig& config)
{
    config_ = config;

    if (const char* frames = std::getenv("VELK_AUTO_EXIT_FRAMES")) {
        auto_exit_frames_ = std::strtol(frames, nullptr, 10);
    }
    if (const char* path = std::getenv("VELK_AUTO_CAPTURE")) {
        auto_capture_path_ = path;
    }
    auto_gpu_stats_ = std::getenv("VELK_AUTO_GPU_STATS") != nullptr;

    auto& v = instance();
    auto& reg = v.plugin_registry();

    // Load all standard plugins. The "plugin:" URI scheme resolves to
    // <exe_dir>/plugins/<name>.<dll|so> on the host platform.
    reg.load_plugin("plugin:velk_ui");
    reg.load_plugin("plugin:velk_render");
    reg.load_plugin("plugin:velk_scene");
    reg.load_plugin("plugin:velk_vk");
    reg.load_plugin("plugin:velk_glsl");
    reg.load_plugin("plugin:velk_rt");
    reg.load_plugin("plugin:velk_text");
    reg.load_plugin("plugin:velk_image");
    reg.load_plugin("plugin:velk_gltf");
    reg.load_plugin("plugin:velk_importer");

    // Load the platform plugin (compile-time selection).
#if defined(__ANDROID__)
    reg.load_plugin("plugin:velk_runtime_android");
    platform_plugin_ = reg.find_plugin(PluginId::RuntimeAndroidPlugin);
#else
    reg.load_plugin("plugin:velk_runtime_glfw");
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
                           const ::velk::IElement::Ptr& camera,
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
    auto* elem = interface_cast<::velk::IElement>(camera);
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
                                       const shared_ptr<::velk::IScene>& scene)
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
    weak_ptr<::velk::IScene> weak_scene(scene);
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
    if (auto_exit_frames_ > 0 && presented_frames_ >= auto_exit_frames_) {
        if (!auto_capture_path_.empty()) {
            capture_first_window();
        }
        for (auto& pass : auto_gpu_samples_) {
            std::sort(pass.ms.begin(), pass.ms.end());
            VELK_LOG(I, "gpu pass %-24s med=%7.3f ms  p90=%7.3f ms  n=%zu", pass.label.c_str(),
                     static_cast<double>(pass.ms[pass.ms.size() / 2]),
                     static_cast<double>(pass.ms[pass.ms.size() * 9 / 10]), pass.ms.size());
        }
        if (auto_gpu_stats_ && renderer_) {
            renderer_->log_gpu_memory();
        }
        return false;
    }
    return window_provider_->poll_events();
}

void Application::capture_first_window()
{
    auto backend = render_ctx_ ? render_ctx_->backend() : nullptr;
    auto* window = windows_.empty() ? nullptr : interface_cast<IWindow>(windows_[0]);
    auto writer = instance().create<IImageWriter>(ui::ClassId::ImageEncoder);
    if (!backend || !window || !writer) {
        VELK_LOG(E, "auto capture: backend, window or image writer missing");
        return;
    }

    // Already acquired by the frame just presented, so this does not clear it.
    auto target = backend->acquire_swapchain_texture(get_render_target_id(window->surface()));
    auto* texture = interface_cast<IGpuTexture>(target.get());
    vector<uint8_t> pixels;
    PixelFormat format{};
    uvec2 dims{};
    if (!texture || !backend->read_texture(*texture, pixels, format, dims)) {
        VELK_LOG(E, "auto capture: could not read the composite");
        return;
    }

    if (format == PixelFormat::RGBA16F) {
        const size_t count = static_cast<size_t>(dims.x) * dims.y;
        const auto* source = reinterpret_cast<const uint16_t*>(pixels.data());
        vector<uint8_t> converted;
        converted.resize(count * 4);
        for (size_t i = 0; i < count * 4; ++i) {
            const float value = (i % 4 == 3)
                ? 1.f
                : std::pow(std::clamp(half_to_float(source[i]), 0.f, 1.f), 1.f / 2.2f);
            converted[i] = static_cast<uint8_t>(value * 255.f + 0.5f);
        }
        pixels = std::move(converted);
        format = PixelFormat::RGBA8;
    }

    const bool saved = writer->save_png(static_cast<int>(dims.x), static_cast<int>(dims.y), format,
                                        pixels.data(), pixels.size(), auto_capture_path_);
    VELK_LOG(I, "auto capture: %s %s", auto_capture_path_.c_str(), saved ? "saved" : "FAILED");
}

void Application::update()
{
    instance().update();
}

::velk::Frame Application::prepare()
{
    if (!renderer_) {
        return {};
    }
    last_prepare_start_ = std::chrono::steady_clock::now();
    auto frame = renderer_->prepare();
    last_prepare_end_ = std::chrono::steady_clock::now();
    return frame;
}

void Application::submit(::velk::Frame frame)
{
    if (!renderer_) {
        return;
    }
    renderer_->present(frame);
    ++presented_frames_;
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
    // Exclude time the renderer spent blocked on GPU completion (the
    // marker wait inside claim_frame_slot) from the overlay's CPU
    // number — that's GPU-bound stalling, not CPU work.
    if (auto* internal = interface_cast<IRendererInternal>(renderer_.get())) {
        cpu_time -= static_cast<double>(internal->consume_last_prepare_gpu_wait_ns()) * 1e-9;
        if (cpu_time < 0.0) cpu_time = 0.0;
    }
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
    // Latest resolved per-pass GPU timings (snapshot of the last completed
    // frame, shared by every overlay). Empty until a timed frame resolves.
    array_view<const GpuPassTiming> gpu_timings;
    if (render_ctx_) {
        if (auto backend = render_ctx_->backend()) {
            if (auto_gpu_stats_) {
                backend->set_gpu_timing_enabled(true);
            }
            gpu_timings = backend->last_gpu_timings();
        }
    }

    // The first frames include pipeline creation, which would skew the medians.
    constexpr long kAutoGpuStatsSkipFrames = 60;
    if (auto_gpu_stats_ && presented_frames_ > kAutoGpuStatsSkipFrames) {
        for (const auto& timing : gpu_timings) {
            const string_view label(timing.label, std::strlen(timing.label));
            AutoGpuPass* found = nullptr;
            for (auto& pass : auto_gpu_samples_) {
                if (string_view(pass.label) == label) {
                    found = &pass;
                    break;
                }
            }
            if (!found) {
                auto_gpu_samples_.push_back({string(label), {}});
                found = &auto_gpu_samples_.back();
            }
            found->ms.push_back(timing.ms);
        }
    }

    for (auto& ov : overlays_) {
        ov.frame_count++;
        ov.cpu_time_accum += cpu_time;
        ov.frame_time_accum += frame_time;

        double elapsed = std::chrono::duration<double>(now - ov.last_update).count();
        if (elapsed >= 0.5) {
            double fps = static_cast<double>(ov.frame_count) / elapsed;
            double avg_cpu_ms = ov.cpu_time_accum / static_cast<double>(ov.frame_count) * 1000.0;
            double avg_frame_ms = ov.frame_time_accum / static_cast<double>(ov.frame_count) * 1000.0;

            // Real GPU total = sum of the measured per-pass durations.
            float gpu_total = 0.f;
            for (const auto& t : gpu_timings) gpu_total += t.ms;

            char buf[512];
            int n = std::snprintf(buf, sizeof(buf),
                "%.0f fps\nframe %.3f ms\ncpu %.3f ms\ngpu %.3f ms",
                fps, avg_frame_ms, avg_cpu_ms, static_cast<double>(gpu_total));
            for (const auto& t : gpu_timings) {
                if (n < 0 || n >= static_cast<int>(sizeof(buf))) break;
                n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                                   "\n  %s %.2f", t.label, static_cast<double>(t.ms));
            }
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

::velk::IRenderer::Ptr Application::renderer() const
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
        // Stop paying for GPU timestamps once no overlay needs them.
        if (overlays_.empty() && render_ctx_) {
            if (auto backend = render_ctx_->backend()) {
                backend->set_gpu_timing_enabled(false);
            }
        }
        return;
    }

    auto* iwin = interface_cast<IWindow>(window);
    if (!iwin || !renderer_) {
        return;
    }

    // The overlay is the only consumer of per-pass GPU timestamps; turn
    // capture on while it's shown (off by default for zero overhead).
    if (render_ctx_) {
        if (auto backend = render_ctx_->backend()) {
            backend->set_gpu_timing_enabled(true);
        }
    }

    // Create the overlay scene with a root element, an ortho camera child,
    // and a text element child holding the FPS text visual.
    auto scene = ::velk::create_scene();
    auto root = ::velk::create_element();
    scene.set_root(root);

    auto camera_elem = ::velk::create_element();
    scene.add(root, camera_elem);

    auto camera_trait = ::velk::trait::render::create_camera();
    camera_trait.set_projection(::velk::Projection::Ortho);
    camera_elem.add_trait(camera_trait);

    auto text_elem = ::velk::create_element();
    scene.add(root, text_elem);

    // Sized to fit the fps/frame/cpu/gpu header plus the per-pass GPU
    // timing breakdown lines (deferred bistro emits ~9 passes).
    auto text_sz = ui::trait::layout::create_fixed_size(::velk::dim::px(224), ::velk::dim::px(248));
    text_elem.add_trait(text_sz);

    auto text_bg = ui::trait::visual::create_rect();
    text_bg.set_color({0.f, 0.f, 0.f, .4f});
    text_elem.add_trait(text_bg);

    auto text_visual = ui::trait::visual::create_text();
    text_visual.set_font(ui::get_default_font());
    text_visual.set_text("FPS: ...");
    text_visual.set_font_size(config.font_size);
    text_visual.set_color(config.text_color);
    text_visual.set_layout(::velk::TextLayout::MultiLine);
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
    bind_scene_to_window(window, iwin, static_cast<::velk::IScene::Ptr>(scene));

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
