#include "android_plugin.h"

#include "android_asset_protocol.h"
#include "android_log_sink.h"
#include "android_window.h"

#include <velk-runtime/plugins/android/native_activity_main.h>

#include <velk/api/velk.h>
#include <velk/api/state.h>
#include <velk/interface/resource/intf_resource_store.h>

#include <velk-ui/api/input_dispatcher.h>

#include <velk-runtime/api/application.h>
#include <velk-scene/interface/intf_renderer.h>

#include <android/choreographer.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <cmath>
#include <dlfcn.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

namespace velk::impl {

namespace {
AAssetManager* g_asset_manager = nullptr;
struct android_app* g_android_app = nullptr;

// AndroidWindow created in wrap_native_surface; the lifecycle event hooks
// fire on the same wrapper so attached event handlers stay registered.
AndroidWindow* g_active_window = nullptr;
ANativeWindow* g_latest_native_window = nullptr;
bool g_running = true;

/// Distance between the first two pointers on the previous frame's MOVE,
/// so we can emit ScrollEvent deltas proportional to pinch distance change.
/// Reset whenever the pinch is interrupted (one finger lifts, gesture cancels).
struct PinchState
{
    float prev_distance = 0.0f;
    bool active = false;
};
PinchState g_pinch;

float pinch_distance(AInputEvent* event)
{
    const float dx = AMotionEvent_getX(event, 1) - AMotionEvent_getX(event, 0);
    const float dy = AMotionEvent_getY(event, 1) - AMotionEvent_getY(event, 0);
    return std::sqrt(dx * dx + dy * dy);
}

int32_t handle_input_event(struct android_app* /*app*/, AInputEvent* event)
{
    if (!g_active_window || !event) {
        return 0;
    }
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        return 0;
    }

    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    const size_t pointer_count = AMotionEvent_getPointerCount(event);

    // Two-finger pinch path: distance change between the first two pointers
    // becomes a ScrollEvent. Scaled into the same magnitude range desktop
    // mouse wheels deliver (~1.0 per "notch") so existing scroll handlers
    // don't need to be touched. Single-finger touch continues to dispatch
    // PointerEvents normally below.
    if (pointer_count >= 2) {
        const float distance = pinch_distance(event);
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN || !g_pinch.active) {
            g_pinch.prev_distance = distance;
            g_pinch.active = true;
            return 1;
        }
        if (action == AMOTION_EVENT_ACTION_MOVE) {
            const float delta = distance - g_pinch.prev_distance;
            constexpr float kPxPerScrollUnit = 60.0f;
            if (std::abs(delta) >= 1.0f) {
                ui::ScrollEvent ev;
                ev.position = {
                    (AMotionEvent_getX(event, 0) + AMotionEvent_getX(event, 1)) * 0.5f,
                    (AMotionEvent_getY(event, 0) + AMotionEvent_getY(event, 1)) * 0.5f,
                };
                ev.delta = {0.0f, delta / kPxPerScrollUnit};
                ev.unit = ui::ScrollUnit::Lines;
                g_active_window->input().scroll_event(ev);
                g_pinch.prev_distance = distance;
            }
            return 1;
        }
        if (action == AMOTION_EVENT_ACTION_POINTER_UP) {
            g_pinch.active = false;
            return 1;
        }
    } else {
        g_pinch.active = false;
    }

    ui::PointerEvent ev;
    ev.position = {AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0)};

    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            // Map touch to right mouse button so the orbit-drag handler in
            // the sample (which keys off PointerButton::Right) works.
            ev.action = ui::PointerAction::Down;
            ev.button = ui::PointerButton::Right;
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            ev.action = ui::PointerAction::Up;
            ev.button = ui::PointerButton::Right;
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            ev.action = ui::PointerAction::Move;
            break;
        default:
            return 0;
    }

    g_active_window->input().pointer_event(ev);
    return 1;
}

void handle_app_cmd(struct android_app* app, int32_t cmd)
{
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            g_latest_native_window = app->window;
            AndroidPlugin::on_init_window(app->window);
            break;
        case APP_CMD_TERM_WINDOW:
            AndroidPlugin::on_term_window();
            g_latest_native_window = nullptr;
            break;
        case APP_CMD_WINDOW_RESIZED:
            if (app->window) {
                AndroidPlugin::on_window_resized(
                    ANativeWindow_getWidth(app->window),
                    ANativeWindow_getHeight(app->window));
            }
            break;
        case APP_CMD_DESTROY:
            g_running = false;
            break;
        default:
            break;
    }
}

// Drain pending NativeActivity events. block==true blocks until at least one
// event arrives; block==false returns immediately if the queue is empty.
void pump_events(bool block)
{
    if (!g_android_app) {
        return;
    }
    int events = 0;
    struct android_poll_source* source = nullptr;
    int timeout = block ? -1 : 0;
    while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        if (source) {
            source->process(g_android_app, source);
        }
        if (g_android_app->destroyRequested) {
            g_running = false;
            return;
        }
        // After the first iteration we don't want to block; just drain.
        timeout = 0;
    }
}

} // namespace

void AndroidPlugin::set_asset_manager(AAssetManager* mgr)
{
    g_asset_manager = mgr;
}

AAssetManager* AndroidPlugin::asset_manager()
{
    return g_asset_manager;
}

void AndroidPlugin::set_android_app(struct android_app* app)
{
    g_android_app = app;
}

struct android_app* AndroidPlugin::android_app_handle()
{
    return g_android_app;
}

void AndroidPlugin::on_init_window(ANativeWindow* w)
{
    if (g_active_window) {
        g_active_window->notify_surface_created(w);
    }
}

void AndroidPlugin::on_term_window()
{
    if (g_active_window) {
        g_active_window->notify_surface_destroyed();
    }
}

void AndroidPlugin::on_window_resized(int width, int height)
{
    if (g_active_window) {
        g_active_window->notify_surface_changed(width, height);
    }
}

ReturnValue AndroidPlugin::initialize(IVelk& velk, PluginConfig&)
{
    // Install logcat sink so VELK_LOG output is visible in `adb logcat`.
    auto sink = ext::make_object<AndroidLogSink>();
    log_sink_ = interface_pointer_cast<ILogSink>(sink);
    velk.log().set_sink(log_sink_);

    // Register app:// protocol backed by AAssetManager. android_main must
    // call set_asset_manager() before plugins load; if it didn't, the
    // protocol resolves nothing.
    auto proto = ext::make_object<AndroidAssetProtocol>();
    auto* concrete = static_cast<AndroidAssetProtocol*>(proto.get());
    concrete->set_asset_manager(g_asset_manager);
    app_protocol_ = interface_pointer_cast<IResourceProtocol>(proto);
    velk.resource_store().register_protocol(app_protocol_);

    return ::velk::register_type<AndroidWindow>(velk);
}

ReturnValue AndroidPlugin::shutdown(IVelk& velk)
{
    if (app_protocol_) {
        velk.resource_store().unregister_protocol(app_protocol_);
        app_protocol_.reset();
    }
    if (log_sink_) {
        velk.log().set_sink({});
        log_sink_.reset();
    }
    return ReturnValue::Success;
}

IWindow::Ptr AndroidPlugin::create_window(const WindowConfig& config,
                                          const IRenderContext::Ptr& ctx)
{
    // Block until NativeActivity hands us an ANativeWindow. On a cold
    // start the first APP_CMD_INIT_WINDOW may not have fired yet by the
    // time the sample asks for a window; pumping the looper here keeps the
    // sample code identical to its desktop shape.
    while (g_running && !g_latest_native_window) {
        pump_events(/*block=*/true);
    }
    if (!g_latest_native_window) {
        return {};
    }

    auto obj = wrap_native_surface(g_latest_native_window, ctx);
    if (!obj) {
        return {};
    }

    auto* win = static_cast<AndroidWindow*>(obj.get());
    win->set_pending_update_rate(config.update_rate);
    win->set_pending_target_fps(config.target_fps);
    win->set_pending_depth(config.depth);
    win->set_pending_color_format(config.color_format);
    g_active_window = win;
    return obj;
}

namespace {

bool create_android_surface(void* vk_instance, void* out_surface, void* user_data)
{
    auto instance = static_cast<VkInstance>(vk_instance);
    auto* surf = static_cast<VkSurfaceKHR*>(out_surface);
    auto* win = static_cast<ANativeWindow*>(user_data);
    if (!instance) {
        VELK_LOG(E, "create_android_surface: VkInstance is null");
        return false;
    }
    if (!surf) {
        VELK_LOG(E, "create_android_surface: out_surface is null");
        return false;
    }
    if (!win) {
        VELK_LOG(E, "create_android_surface: ANativeWindow* (user_data) is null");
        return false;
    }

    // velk_vk uses VK_NO_PROTOTYPES + volk; volk dlopens libvulkan.so but
    // doesn't necessarily export vkGetInstanceProcAddr into RTLD_DEFAULT.
    // Open libvulkan.so explicitly here to be sure.
    void* libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libvulkan) {
        libvulkan = dlopen("libvulkan.so", RTLD_NOW);
    }
    if (!libvulkan) {
        VELK_LOG(E, "create_android_surface: dlopen(libvulkan.so) failed: %s",
                 dlerror() ? dlerror() : "(no error)");
        return false;
    }
    auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(libvulkan, "vkGetInstanceProcAddr"));
    if (!get_proc) {
        VELK_LOG(E, "create_android_surface: dlsym(vkGetInstanceProcAddr) failed: %s",
                 dlerror() ? dlerror() : "(no error)");
        return false;
    }
    auto fn = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        get_proc(instance, "vkCreateAndroidSurfaceKHR"));
    if (!fn) {
        VELK_LOG(E, "create_android_surface: vkGetInstanceProcAddr(vkCreateAndroidSurfaceKHR) returned null"
                    " — instance may not have VK_KHR_android_surface enabled");
        return false;
    }

    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = win;
    VkResult r = fn(instance, &ci, nullptr, surf);
    if (r != VK_SUCCESS) {
        VELK_LOG(E, "create_android_surface: vkCreateAndroidSurfaceKHR failed (VkResult=%d)",
                 static_cast<int>(r));
        return false;
    }
    return true;
}

} // namespace

IWindow::Ptr AndroidPlugin::wrap_native_surface(void* native_handle,
                                                const IRenderContext::Ptr& ctx)
{
    auto* native = static_cast<ANativeWindow*>(native_handle);
    if (!native) {
        return {};
    }

    auto obj = instance().create<IWindow>(ClassId::AndroidWindow);
    if (!obj) {
        return {};
    }

    auto* win = static_cast<AndroidWindow*>(obj.get());
    win->set_native_window(native);

    auto dispatcher = instance().create<ui::IInputDispatcher>(ui::ClassId::Input::Dispatcher);
    win->set_input_dispatcher(std::move(dispatcher));

    if (ctx) {
        SurfaceConfig sc;
        sc.width = ANativeWindow_getWidth(native);
        sc.height = ANativeWindow_getHeight(native);
        auto surface = ctx->create_surface(sc);
        win->set_surface(std::move(surface));
        win->set_render_context(ctx);
    } else {
        // First window: hand the ANativeWindow* off to velk_vk via the
        // VulkanInitParams::create_surface callback. The backend invokes it
        // during vkCreateAndroidSurfaceKHR.
        vk_params_.user_data = native;
        vk_params_.create_surface = &create_android_surface;
    }

    return obj;
}

void AndroidPlugin::finalize_window(const IWindow::Ptr& window,
                                    const IRenderContext::Ptr& ctx)
{
    if (!ctx || !window) {
        return;
    }
    auto* win = static_cast<AndroidWindow*>(window.get());
    auto* native = win->native_window();
    if (!native) {
        return;
    }
    SurfaceConfig sc;
    sc.width = ANativeWindow_getWidth(native);
    sc.height = ANativeWindow_getHeight(native);
    sc.update_rate = win->pending_update_rate();
    sc.target_fps = win->pending_target_fps();
    sc.depth = win->pending_depth();
    sc.color_format = win->pending_color_format();
    auto surface = ctx->create_surface(sc);
    win->set_surface(std::move(surface));
    win->set_render_context(ctx);
}

bool AndroidPlugin::poll_events()
{
    pump_events(/*block=*/false);
    return g_running;
}

void* AndroidPlugin::get_backend_params()
{
    return &vk_params_;
}

} // namespace velk::impl

namespace velk {

void android_native_activity_run(struct android_app* app, int (*run_fn)())
{
    impl::AndroidPlugin::set_android_app(app);
    if (app && app->activity) {
        impl::AndroidPlugin::set_asset_manager(app->activity->assetManager);
    }
    app->onAppCmd = &impl::handle_app_cmd;
    app->onInputEvent = &impl::handle_input_event;

    // Drain any commands already queued before the user code runs; this
    // typically includes the first APP_CMD_INPUT_CHANGED but not yet
    // APP_CMD_INIT_WINDOW (that arrives once the Java surface is ready).
    impl::pump_events(/*block=*/false);

    if (run_fn) {
        run_fn();
    }
}

namespace {

struct FrameLoopState
{
    Application app;
    AChoreographer* choreographer = nullptr;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::queue<::velk::Frame> frame_queue;
    std::atomic<bool> stop{false};
};

void on_choreographer_frame(long /*frame_time_nanos*/, void* data)
{
    auto* s = static_cast<FrameLoopState*>(data);
    if (s->stop.load()) {
        return;
    }
    s->app.update();
    auto frame = s->app.prepare();
    {
        std::lock_guard<std::mutex> lk(s->frame_mutex);
        s->frame_queue.push(frame);
    }
    s->frame_cv.notify_one();

    AChoreographer_postFrameCallback(s->choreographer, &on_choreographer_frame, s);
}

} // namespace

void android_run_frame_loop(Application app)
{
    auto* a = impl::AndroidPlugin::android_app_handle();
    if (!a) {
        return;
    }

    FrameLoopState state;
    state.app = std::move(app);
    state.choreographer = AChoreographer_getInstance();
    if (!state.choreographer) {
        return;
    }

    // Render thread: drains prepared frames and calls submit.
    std::thread render_thread([&state] {
        while (true) {
            ::velk::Frame frame;
            {
                std::unique_lock<std::mutex> lk(state.frame_mutex);
                state.frame_cv.wait(lk, [&] {
                    return state.stop.load() || !state.frame_queue.empty();
                });
                if (state.stop.load() && state.frame_queue.empty()) {
                    return;
                }
                frame = state.frame_queue.front();
                state.frame_queue.pop();
            }
            state.app.submit(frame);
        }
    });

    // First frame callback. Subsequent ones re-arm themselves from inside
    // on_choreographer_frame.
    AChoreographer_postFrameCallback(state.choreographer, &on_choreographer_frame, &state);

    // Main thread loop: pump Looper indefinitely. Both NativeActivity events
    // (window lifecycle, input) and Choreographer callbacks land here.
    while (!state.stop.load() && !a->destroyRequested) {
        int events = 0;
        struct android_poll_source* source = nullptr;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) {
                source->process(a, source);
            }
        }
    }

    // Shut down the render thread cleanly.
    state.stop.store(true);
    state.frame_cv.notify_all();
    if (render_thread.joinable()) {
        render_thread.join();
    }
}

} // namespace velk
