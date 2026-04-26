#include "glfw_window.h"

#include <velk/api/event.h>
#include <velk/api/state.h>

namespace velk::impl {

GlfwWindow::~GlfwWindow()
{
    if (window_) {
        glfwSetWindowUserPointer(window_, nullptr);
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
}

void GlfwWindow::set_glfw_handle(GLFWwindow* window)
{
    window_ = window;
    if (window_) {
        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebuffer_size_callback);
        glfwSetCursorPosCallback(window_, cursor_pos_callback);
        glfwSetMouseButtonCallback(window_, mouse_button_callback);
        glfwSetScrollCallback(window_, scroll_callback);
        glfwSetKeyCallback(window_, key_callback);
    }
}

void GlfwWindow::set_external_handle(void* handle)
{
    external_handle_ = handle;
}

void GlfwWindow::set_surface(IWindowSurface::Ptr surface)
{
    surface_ = std::move(surface);
}

void GlfwWindow::set_render_context(const IRenderContext::Ptr& ctx)
{
    render_ctx_ = ctx;
}

void GlfwWindow::set_input_dispatcher(ui::IInputDispatcher::Ptr dispatcher)
{
    input_ = std::move(dispatcher);
}

IWindowSurface::Ptr GlfwWindow::surface() const
{
    return surface_;
}

ui::IInputDispatcher& GlfwWindow::input() const
{
    return *input_;
}

IRenderContext::Ptr GlfwWindow::render_context() const
{
    return render_ctx_.lock();
}

bool GlfwWindow::should_close() const
{
    // External (framework-owned) windows: lifecycle managed by the platform.
    if (external_handle_) {
        return false;
    }
    return window_ && glfwWindowShouldClose(window_);
}

void GlfwWindow::framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }

    ::velk::size new_size{static_cast<float>(width), static_cast<float>(height)};

    // Update window size property.
    write_state<IWindow>(self, [&](IWindow::State& s) {
        s.size = new_size;
    });

    // Update surface dimensions.
    if (self->surface_) {
        write_state<IWindowSurface>(self->surface_, [&](IWindowSurface::State& s) {
            s.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        });
    }

    ::velk::invoke_event(self->get_interface(IInterface::UID), "on_resize", new_size);
}

void GlfwWindow::cursor_pos_callback(GLFWwindow* window, double x, double y)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self || !self->input_) {
        return;
    }

    ui::PointerEvent ev;
    ev.position = {static_cast<float>(x), static_cast<float>(y)};
    ev.action = ui::PointerAction::Move;
    self->input_->pointer_event(ev);
}

void GlfwWindow::mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self || !self->input_) {
        return;
    }

    ui::PointerEvent ev;
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    ev.position = {static_cast<float>(mx), static_cast<float>(my)};
    ev.action = (action == GLFW_PRESS) ? ui::PointerAction::Down : ui::PointerAction::Up;
    ev.button = (button == GLFW_MOUSE_BUTTON_LEFT)    ? ui::PointerButton::Left
                : (button == GLFW_MOUSE_BUTTON_RIGHT) ? ui::PointerButton::Right
                                                      : ui::PointerButton::Middle;
    if (mods & GLFW_MOD_SHIFT) {
        ev.modifiers = ev.modifiers | ui::Modifier::Shift;
    }
    if (mods & GLFW_MOD_CONTROL) {
        ev.modifiers = ev.modifiers | ui::Modifier::Ctrl;
    }
    if (mods & GLFW_MOD_ALT) {
        ev.modifiers = ev.modifiers | ui::Modifier::Alt;
    }
    if (mods & GLFW_MOD_SUPER) {
        ev.modifiers = ev.modifiers | ui::Modifier::Super;
    }
    self->input_->pointer_event(ev);
}

void GlfwWindow::scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self || !self->input_) {
        return;
    }

    ui::ScrollEvent ev;
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    ev.position = {static_cast<float>(mx), static_cast<float>(my)};
    ev.delta = {static_cast<float>(xoffset), static_cast<float>(yoffset)};
    ev.unit = ui::ScrollUnit::Lines;
    self->input_->scroll_event(ev);
}

void GlfwWindow::key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self || !self->input_) {
        return;
    }

    ui::KeyEvent ev;
    ev.key = key;
    ev.scancode = scancode;
    ev.action = (action == GLFW_PRESS)    ? ui::KeyAction::Down
                : (action == GLFW_REPEAT) ? ui::KeyAction::Repeat
                                          : ui::KeyAction::Up;
    if (mods & GLFW_MOD_SHIFT)   ev.modifiers = ev.modifiers | ui::Modifier::Shift;
    if (mods & GLFW_MOD_CONTROL) ev.modifiers = ev.modifiers | ui::Modifier::Ctrl;
    if (mods & GLFW_MOD_ALT)     ev.modifiers = ev.modifiers | ui::Modifier::Alt;
    if (mods & GLFW_MOD_SUPER)   ev.modifiers = ev.modifiers | ui::Modifier::Super;
    self->input_->key_event(ev);
}

} // namespace velk::impl
