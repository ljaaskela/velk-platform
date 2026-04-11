#include "glfw_plugin.h"

#include "glfw_window.h"

#include <velk/api/velk.h>

#include <velk-ui/api/input_dispatcher.h>

#if defined(APIENTRY)
#undef APIENTRY
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#endif

namespace velk::impl {

static void glfw_error_callback(int error, const char* description)
{
    VELK_LOG(E, "GLFW error %d: %s", error, description);
}

ReturnValue GlfwPlugin::initialize(IVelk& velk, PluginConfig&)
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        VELK_LOG(E, "Failed to initialize GLFW");
        return ReturnValue::Fail;
    }

    return ::velk::register_type<GlfwWindow>(velk);
}

ReturnValue GlfwPlugin::shutdown(IVelk&)
{
    windows_.clear();
    glfwTerminate();
    return ReturnValue::Success;
}

GlfwWindow* GlfwPlugin::get_glfw_window(const IWindow::Ptr& w) const
{
    return static_cast<GlfwWindow*>(w.get());
}

IWindow::Ptr GlfwPlugin::create_window(const WindowConfig& config,
                                       const IRenderContext::Ptr& ctx)
{
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow* glfw_win = glfwCreateWindow(
        config.width, config.height,
        string(config.title).c_str(),
        nullptr, nullptr);

    if (!glfw_win) {
        VELK_LOG(E, "Failed to create GLFW window");
        return {};
    }

    auto obj = instance().create<IWindow>(ClassId::GlfwWindow);
    if (!obj) {
        glfwDestroyWindow(glfw_win);
        return {};
    }

    auto* win = get_glfw_window(obj);
    win->set_glfw_handle(glfw_win);

    write_state<IWindow>(win, [&](IWindow::State& s) {
        s.size = {static_cast<float>(config.width), static_cast<float>(config.height)};
    });

    auto dispatcher = instance().create<ui::IInputDispatcher>(ui::ClassId::Input::Dispatcher);
    win->set_input_dispatcher(std::move(dispatcher));

    win->set_pending_update_rate(config.update_rate);
    win->set_pending_target_fps(config.target_fps);

    if (ctx) {
        SurfaceConfig sc;
        sc.width = config.width;
        sc.height = config.height;
        sc.update_rate = config.update_rate;
        sc.target_fps = config.target_fps;
        auto surface = ctx->create_surface(sc);
        win->set_surface(std::move(surface));
        win->set_render_context(ctx);
    } else {
        // First window: set up VulkanInitParams for later render context creation.
        vk_params_.user_data = glfw_win;
        vk_params_.create_surface = [](void* vk_instance, void* out_surface, void* user_data) -> bool {
            auto inst = static_cast<VkInstance>(vk_instance);
            auto* surf = static_cast<VkSurfaceKHR*>(out_surface);
            auto* w = static_cast<GLFWwindow*>(user_data);
            return glfwCreateWindowSurface(inst, w, nullptr, surf) == VK_SUCCESS;
        };
    }

    windows_.emplace_back(obj);
    return obj;
}

IWindow::Ptr GlfwPlugin::wrap_native_surface(void* native_handle,
                                              const IRenderContext::Ptr& ctx)
{
    if (!native_handle) {
        return {};
    }

#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!IsWindow(hwnd)) {
        VELK_LOG(E, "wrap_native_surface: invalid HWND");
        return {};
    }

    RECT rect{};
    GetClientRect(hwnd, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    auto obj = instance().create<IWindow>(ClassId::GlfwWindow);
    if (!obj) {
        return {};
    }

    auto* win = get_glfw_window(obj);
    win->set_external_handle(hwnd);

    write_state<IWindow>(win, [&](IWindow::State& s) {
        s.size = {static_cast<float>(w), static_cast<float>(h)};
    });

    auto dispatcher = instance().create<ui::IInputDispatcher>(ui::ClassId::Input::Dispatcher);
    win->set_input_dispatcher(std::move(dispatcher));

    if (ctx) {
        SurfaceConfig sc;
        sc.width = w;
        sc.height = h;
        // Wrapped native surfaces default to VSync; framework usually controls pacing.
        auto surface = ctx->create_surface(sc);
        win->set_surface(std::move(surface));
        win->set_render_context(ctx);
    } else {
        // First window: stash HWND for the create_surface callback.
        vk_params_.user_data = hwnd;
        vk_params_.create_surface = [](void* vk_instance, void* out_surface, void* user_data) -> bool {
            auto inst = static_cast<VkInstance>(vk_instance);
            auto* surf = static_cast<VkSurfaceKHR*>(out_surface);
            auto h = static_cast<HWND>(user_data);

            auto fn = (PFN_vkCreateWin32SurfaceKHR)
                glfwGetInstanceProcAddress(inst, "vkCreateWin32SurfaceKHR");
            if (!fn) {
                return false;
            }
            VkWin32SurfaceCreateInfoKHR ci{};
            ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            ci.hwnd = h;
            ci.hinstance = GetModuleHandleW(nullptr);
            return fn(inst, &ci, nullptr, surf) == VK_SUCCESS;
        };
    }

    windows_.emplace_back(obj);
    return obj;
#else
    (void)ctx;
    VELK_LOG(E, "wrap_native_surface: not implemented on this platform");
    return {};
#endif
}

void GlfwPlugin::finalize_window(const IWindow::Ptr& window,
                                 const IRenderContext::Ptr& ctx)
{
    if (!ctx || !window) {
        return;
    }

    auto* win = get_glfw_window(window);
    auto state = read_state<IWindow>(win);

    SurfaceConfig sc;
    sc.width = static_cast<int>(state->size.width);
    sc.height = static_cast<int>(state->size.height);
    sc.update_rate = win->pending_update_rate();
    sc.target_fps = win->pending_target_fps();

    auto surface = ctx->create_surface(sc);
    win->set_surface(std::move(surface));
    win->set_render_context(ctx);
}

bool GlfwPlugin::poll_events()
{
    glfwPollEvents();

    // Return false if all windows should close.
    bool any_alive = false;
    for (auto& w : windows_) {
        if (w && !w->should_close()) {
            any_alive = true;
            break;
        }
    }
    return any_alive;
}

void* GlfwPlugin::get_backend_params()
{
    return &vk_params_;
}

} // namespace velk::impl
