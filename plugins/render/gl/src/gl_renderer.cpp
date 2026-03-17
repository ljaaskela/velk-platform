#include "gl_renderer.h"

#include <velk/api/callback.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <glad/gl.h>

#include <cstring>

namespace velk_ui {

namespace {

const char* vertex_shader_src = R"(
#version 330 core

// Unit quad: 4 vertices forming a triangle strip
const vec2 quad[4] = vec2[4](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

// Per-instance attributes
layout(location = 0) in vec4 inst_rect;  // x, y, width, height
layout(location = 1) in vec4 inst_color; // r, g, b, a

uniform mat4 u_projection;

out vec4 v_color;

void main()
{
    vec2 pos = quad[gl_VertexID];
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
}
)";

const char* fragment_shader_src = R"(
#version 330 core

in vec4 v_color;
out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

GLuint compile_shader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link_program(GLuint vert, GLuint frag)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

} // namespace

GlRenderer::~GlRenderer()
{
    if (initialized_) {
        GlRenderer::shutdown();
    }
}

bool GlRenderer::init(int width, int height)
{
    // Assumes the caller has already loaded GL function pointers (e.g. via gladLoadGL).

    // Compile shaders
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }

    shader_program_ = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!shader_program_) {
        return false;
    }

    uniform_projection_ = glGetUniformLocation(shader_program_, "u_projection");

    // Create VAO
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    // Instance VBO
    glGenBuffers(1, &vbo_instance_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_instance_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

    // Per-instance rect (location 0): x, y, width, height
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData),
                          reinterpret_cast<void*>(offsetof(InstanceData, x)));
    glVertexAttribDivisor(0, 1);

    // Per-instance color (location 1): r, g, b, a
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData),
                          reinterpret_cast<void*>(offsetof(InstanceData, r)));
    glVertexAttribDivisor(1, 1);

    glBindVertexArray(0);

    // Set viewport
    glViewport(0, 0, width, height);

    // Store viewport dimensions
    {
        auto state = velk::write_state<IRenderer>(this);
        if (state) {
            state->viewport_width = static_cast<uint32_t>(width);
            state->viewport_height = static_cast<uint32_t>(height);
        }
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    initialized_ = true;
    return true;
}

void GlRenderer::render()
{
    if (!initialized_) return;

    auto reader = velk::read_state<IRenderer>(this);
    uint32_t vw = reader ? reader->viewport_width : 0;
    uint32_t vh = reader ? reader->viewport_height : 0;

    // Upload dirty slots
    if (!dirty_slots_.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_instance_);
        for (uint32_t slot : dirty_slots_) {
            glBufferSubData(GL_ARRAY_BUFFER,
                            static_cast<GLintptr>(slot * sizeof(InstanceData)),
                            sizeof(InstanceData),
                            &cpu_buffer_[slot]);
        }
        dirty_slots_.clear();
    }

    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (cpu_buffer_.empty()) return;

    glUseProgram(shader_program_);

    // Orthographic projection: (0,0) top-left, (vw, vh) bottom-right
    float L = 0.0f;
    float R = static_cast<float>(vw);
    float T = 0.0f;
    float B = static_cast<float>(vh);
    // Column-major ortho matrix
    float projection[16] = {
        2.0f / (R - L),     0.0f,               0.0f, 0.0f,
        0.0f,               2.0f / (T - B),     0.0f, 0.0f,
        0.0f,               0.0f,              -1.0f, 0.0f,
        -(R + L) / (R - L), -(T + B) / (T - B), 0.0f, 1.0f,
    };
    glUniformMatrix4fv(uniform_projection_, 1, GL_FALSE, projection);

    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(cpu_buffer_.size()));
    glBindVertexArray(0);
    glUseProgram(0);
}

void GlRenderer::shutdown()
{
    if (!initialized_) return;

    // Unsubscribe all listeners
    for (auto& entry : entries_) {
        entry.listeners.clear();
        entry.element = nullptr;
    }
    entries_.clear();
    cpu_buffer_.clear();
    free_slots_.clear();
    dirty_slots_.clear();

    if (shader_program_) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (vbo_instance_) {
        glDeleteBuffers(1, &vbo_instance_);
        vbo_instance_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }

    initialized_ = false;
}

void GlRenderer::sync_slot(uint32_t slot, velk::IObject* element)
{
    auto state = velk::read_state<IElement>(element);
    if (state) {
        auto& inst = cpu_buffer_[slot];
        inst.x = state->x;
        inst.y = state->y;
        inst.width = state->width;
        inst.height = state->height;
        inst.r = state->r;
        inst.g = state->g;
        inst.b = state->b;
        inst.a = state->a;
    }
}

IRenderer::VisualId GlRenderer::add_visual(velk::IObject::Ptr element)
{
    if (!element) return 0;

    uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<uint32_t>(cpu_buffer_.size());
        cpu_buffer_.push_back({});
        entries_.push_back({});

        // Resize GPU buffer
        if (initialized_) {
            glBindBuffer(GL_ARRAY_BUFFER, vbo_instance_);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(cpu_buffer_.size() * sizeof(InstanceData)),
                         nullptr, GL_DYNAMIC_DRAW);
            // Mark all existing slots dirty so they get re-uploaded
            for (uint32_t i = 0; i < static_cast<uint32_t>(cpu_buffer_.size()); ++i) {
                if (entries_[i].alive) {
                    dirty_slots_.push_back(i);
                    entries_[i].dirty = true;
                }
            }
        }
    }

    auto& entry = entries_[slot];
    entry.element = element;
    entry.alive = true;
    entry.dirty = true;

    // Read initial state
    sync_slot(slot, element.get());

    // Subscribe to property changes via on_changed on each IElement property
    auto* meta = velk::interface_cast<velk::IMetadata>(element);
    if (meta) {
        // Subscribe to each IElement property's on_changed
        static constexpr velk::string_view prop_names[] = {
            "x", "y", "width", "height", "r", "g", "b", "a"};

        velk::IObject::WeakPtr weak_element(element);
        for (auto& name : prop_names) {
            auto prop = meta->get_property(name, velk::Resolve::Existing);
            if (!prop) continue;

            // Capture slot by value. The callback reads state and patches cpu_buffer_.
            velk::Callback cb([this, slot, weak_element](velk::FnArgs) -> velk::ReturnValue {
                auto locked = weak_element.lock();
                if (!locked || slot >= entries_.size() || !entries_[slot].alive) {
                    return velk::ReturnValue::Fail;
                }
                sync_slot(slot, locked.get());
                if (!entries_[slot].dirty) {
                    entries_[slot].dirty = true;
                    dirty_slots_.push_back(slot);
                }
                return velk::ReturnValue::Success;
            });

            velk::IFunction::ConstPtr fn_ptr = cb;
            prop->on_changed()->add_handler(fn_ptr);
            entry.listeners.push_back(std::move(fn_ptr));
        }
    }

    dirty_slots_.push_back(slot);

    return slot + 1; // VisualId 0 = invalid
}

void GlRenderer::remove_visual(VisualId id)
{
    if (id == 0 || id > entries_.size()) return;

    uint32_t slot = id - 1;
    auto& entry = entries_[slot];
    if (!entry.alive) return;

    // Unsubscribe listeners
    if (entry.element) {
        auto* meta = velk::interface_cast<velk::IMetadata>(entry.element);
        if (meta) {
            static constexpr velk::string_view prop_names[] = {
                "x", "y", "width", "height", "r", "g", "b", "a"};
            size_t listener_idx = 0;
            for (auto& name : prop_names) {
                auto prop = meta->get_property(name, velk::Resolve::Existing);
                if (prop && listener_idx < entry.listeners.size()) {
                    prop->on_changed()->remove_handler(entry.listeners[listener_idx]);
                }
                ++listener_idx;
            }
        }
    }

    entry.listeners.clear();
    entry.element = nullptr;
    entry.alive = false;
    entry.dirty = true;

    // Zero out the slot (degenerate, won't render)
    std::memset(&cpu_buffer_[slot], 0, sizeof(InstanceData));
    dirty_slots_.push_back(slot);

    free_slots_.push_back(slot);
}

} // namespace velk_ui
