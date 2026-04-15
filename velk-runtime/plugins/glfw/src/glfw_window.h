#ifndef VELK_RUNTIME_GLFW_WINDOW_IMPL_H
#define VELK_RUNTIME_GLFW_WINDOW_IMPL_H

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <velk/ext/object.h>

#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-runtime/interface/intf_window.h>
#include <velk-runtime/plugins/glfw/plugin.h>
#include <velk-ui/interface/intf_input_dispatcher.h>

namespace velk::impl {

class GlfwWindow : public ext::Object<GlfwWindow, IWindow>
{
public:
    VELK_CLASS_UID(ClassId::GlfwWindow, "GlfwWindow");

    ~GlfwWindow();

    void set_glfw_handle(GLFWwindow* window);
    void set_external_handle(void* handle);
    void set_surface(IWindowSurface::Ptr surface);
    void set_render_context(const IRenderContext::Ptr& ctx);
    void set_input_dispatcher(ui::IInputDispatcher::Ptr dispatcher);
    void set_pending_update_rate(UpdateRate r) { pending_update_rate_ = r; }
    void set_pending_target_fps(int fps) { pending_target_fps_ = fps; }

    GLFWwindow* glfw_handle() const { return window_; }
    void* external_handle() const { return external_handle_; }
    UpdateRate pending_update_rate() const { return pending_update_rate_; }
    int pending_target_fps() const { return pending_target_fps_; }

    IWindowSurface::Ptr surface() const override;
    ui::IInputDispatcher& input() const override;
    IRenderContext::Ptr render_context() const override;
    bool should_close() const override;

private:
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
    static void cursor_pos_callback(GLFWwindow* window, double x, double y);
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);

    GLFWwindow* window_ = nullptr;
    void* external_handle_ = nullptr;
    IWindowSurface::Ptr surface_;
    IRenderContext::WeakPtr render_ctx_;
    ui::IInputDispatcher::Ptr input_;
    UpdateRate pending_update_rate_ = UpdateRate::VSync;
    int pending_target_fps_ = 60;
};

} // namespace velk::impl

#endif // VELK_RUNTIME_GLFW_WINDOW_IMPL_H
