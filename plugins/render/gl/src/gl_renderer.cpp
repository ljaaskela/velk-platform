#include "gl_renderer.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <cstring>
#include <glad/gl.h>
#include <velk-ui/interface/intf_material.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk_ui {

namespace {

const char* rect_vertex_src = R"(
#version 330 core

const vec2 quad[4] = vec2[4](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;

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

const char* rect_fragment_src = R"(
#version 330 core

in vec4 v_color;
out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

const char* text_vertex_src = R"(
#version 330 core

const vec2 quad[4] = vec2[4](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;
layout(location = 2) in vec4 inst_uv;   // u0, v0, u1, v1

uniform mat4 u_projection;

out vec4 v_color;
out vec2 v_uv;

void main()
{
    vec2 pos = quad[gl_VertexID];
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
    v_uv.x = mix(inst_uv.x, inst_uv.z, pos.x);
    v_uv.y = mix(inst_uv.y, inst_uv.w, pos.y);
}
)";

const char* text_fragment_src = R"(
#version 330 core

uniform sampler2D u_atlas;

in vec4 v_color;
in vec2 v_uv;
out vec4 frag_color;

void main()
{
    float alpha = texture(u_atlas, v_uv).r;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
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
        VELK_LOG(E, "Shader compile error: %s", log);
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
        VELK_LOG(E, "Program link error: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint build_program(const char* vert_src, const char* frag_src)
{
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vert || !frag) {
        if (vert) {
            glDeleteShader(vert);
        }
        if (frag) {
            glDeleteShader(frag);
        }
        return 0;
    }
    GLuint program = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
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
    // Build rect shader program
    rect_program_ = build_program(rect_vertex_src, rect_fragment_src);
    if (!rect_program_) {
        return false;
    }
    rect_proj_uniform_ = glGetUniformLocation(rect_program_, "u_projection");

    // Build text shader program
    text_program_ = build_program(text_vertex_src, text_fragment_src);
    if (!text_program_) {
        return false;
    }
    text_proj_uniform_ = glGetUniformLocation(text_program_, "u_projection");
    text_atlas_uniform_ = glGetUniformLocation(text_program_, "u_atlas");

    // Rect VAO + VBO
    glGenVertexArrays(1, &rect_vao_);
    glBindVertexArray(rect_vao_);
    glGenBuffers(1, &rect_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, rect_vbo_);

    // location 0: inst_rect (x, y, width, height)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(RectInstanceData),
                          reinterpret_cast<void*>(offsetof(RectInstanceData, x)));
    glVertexAttribDivisor(0, 1);

    // location 1: inst_color (r, g, b, a)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(RectInstanceData),
                          reinterpret_cast<void*>(offsetof(RectInstanceData, color)));
    glVertexAttribDivisor(1, 1);

    glBindVertexArray(0);

    // Text VAO + VBO
    glGenVertexArrays(1, &text_vao_);
    glBindVertexArray(text_vao_);
    glGenBuffers(1, &text_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, text_vbo_);

    // location 0: inst_rect (x, y, width, height)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(TextInstanceData),
                          reinterpret_cast<void*>(offsetof(TextInstanceData, x)));
    glVertexAttribDivisor(0, 1);

    // location 1: inst_color (r, g, b, a)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(TextInstanceData),
                          reinterpret_cast<void*>(offsetof(TextInstanceData, color)));
    glVertexAttribDivisor(1, 1);

    // location 2: inst_uv (u0, v0, u1, v1)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(TextInstanceData),
                          reinterpret_cast<void*>(offsetof(TextInstanceData, u0)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);

    // Create atlas texture (initially empty)
    glGenTextures(1, &atlas_texture_);
    glBindTexture(GL_TEXTURE_2D, atlas_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Viewport
    glViewport(0, 0, width, height);

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
    if (!initialized_) {
        return;
    }

    // Upload atlas only when flagged dirty
    if (atlas_dirty_) {
        upload_atlas();
    }

    // Rebuild instance buffers only when something changed
    if (instances_dirty_) {
        rebuild_instances();
        instances_dirty_ = false;
    }

    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    auto reader = velk::read_state<IRenderer>(this);
    uint32_t vw = reader ? reader->viewport_width : 0;
    uint32_t vh = reader ? reader->viewport_height : 0;

    // Orthographic projection: (0,0) top-left, (vw, vh) bottom-right
    float L = 0.0f;
    float R = static_cast<float>(vw);
    float T = 0.0f;
    float B = static_cast<float>(vh);
    float projection[16] = {
        2.0f / (R - L),
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        2.0f / (T - B),
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        -1.0f,
        0.0f,
        -(R + L) / (R - L),
        -(T + B) / (T - B),
        0.0f,
        1.0f,
    };

    // Pass 1: untextured rectangles
    if (!rect_instances_.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, rect_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(rect_instances_.size() * sizeof(RectInstanceData)),
                     rect_instances_.data(),
                     GL_STREAM_DRAW);

        glUseProgram(rect_program_);
        glUniformMatrix4fv(rect_proj_uniform_, 1, GL_FALSE, projection);
        glBindVertexArray(rect_vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(rect_instances_.size()));
        glBindVertexArray(0);
    }

    // Pass 1b: custom shader rectangles (one draw call per rect)
    for (auto& csr : custom_shader_rects_) {
        glBindBuffer(GL_ARRAY_BUFFER, rect_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sizeof(RectInstanceData)),
                     &csr.instance,
                     GL_STREAM_DRAW);

        glUseProgram(csr.program);
        auto proj_loc = glGetUniformLocation(csr.program, "u_projection");
        glUniformMatrix4fv(proj_loc, 1, GL_FALSE, projection);

        // Pass element rect as uniform for SDF/procedural effects
        auto rect_loc = glGetUniformLocation(csr.program, "u_rect");
        if (rect_loc >= 0) {
            glUniform4f(rect_loc, csr.instance.x, csr.instance.y,
                        csr.instance.width, csr.instance.height);
        }

        glBindVertexArray(rect_vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
        glBindVertexArray(0);
    }

    // Pass 2: textured quads (text)
    if (!text_instances_.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, text_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(text_instances_.size() * sizeof(TextInstanceData)),
                     text_instances_.data(),
                     GL_STREAM_DRAW);

        glUseProgram(text_program_);
        glUniformMatrix4fv(text_proj_uniform_, 1, GL_FALSE, projection);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas_texture_);
        glUniform1i(text_atlas_uniform_, 0);

        glBindVertexArray(text_vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(text_instances_.size()));
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glUseProgram(0);
}

void GlRenderer::shutdown()
{
    if (!initialized_) {
        return;
    }

    for (auto& entry : entries_) {
        entry.element = nullptr;
        entry.cached_commands.clear();
    }
    entries_.clear();
    free_slots_.clear();
    object_to_slot_.clear();
    rect_instances_.clear();
    text_instances_.clear();

    for (auto& entry : entries_) {
        if (entry.custom_program) {
            glDeleteProgram(entry.custom_program);
            entry.custom_program = 0;
        }
    }
    if (rect_program_) {
        glDeleteProgram(rect_program_);
        rect_program_ = 0;
    }
    if (text_program_) {
        glDeleteProgram(text_program_);
        text_program_ = 0;
    }
    if (rect_vbo_) {
        glDeleteBuffers(1, &rect_vbo_);
        rect_vbo_ = 0;
    }
    if (text_vbo_) {
        glDeleteBuffers(1, &text_vbo_);
        text_vbo_ = 0;
    }
    if (rect_vao_) {
        glDeleteVertexArrays(1, &rect_vao_);
        rect_vao_ = 0;
    }
    if (text_vao_) {
        glDeleteVertexArrays(1, &text_vao_);
        text_vao_ = 0;
    }
    if (atlas_texture_) {
        glDeleteTextures(1, &atlas_texture_);
        atlas_texture_ = 0;
    }

    initialized_ = false;
}

void GlRenderer::rebuild_commands(uint32_t slot)
{
    auto& entry = entries_[slot];
    if (!entry.is_valid()) {
        return;
    }

    entry.cached_commands.clear();
    entry.texture_provider = nullptr;
    if (entry.custom_program) {
        glDeleteProgram(entry.custom_program);
        entry.custom_program = 0;
    }

    auto* storage = interface_cast<velk::IObjectStorage>(entry.element);
    if (!storage) {
        return;
    }

    auto state = velk::read_state<IElement>(entry.element);
    if (!state) {
        return;
    }

    velk::rect local_rect = {0, 0, state->size.width, state->size.height};

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);

        auto* visual = interface_cast<IVisual>(att);
        if (visual) {
            auto commands = visual->get_draw_commands(local_rect);
            for (auto& cmd : commands) {
                entry.cached_commands.push_back(cmd);
            }

            // Check for custom material shader via state
            auto vstate = velk::read_state<IVisual>(visual);
            auto mat_obj = (vstate && vstate->paint) ? vstate->paint.get() : velk::IObject::Ptr{};
            auto* mat = mat_obj ? interface_cast<IMaterial>(mat_obj) : nullptr;
            if (mat) {
                auto mstate = velk::read_state<IMaterial>(mat);
                if (mstate && !mstate->fragment_source.empty()) {
                    auto prog = build_program(rect_vertex_src, mstate->fragment_source.c_str());
                    if (prog) {
                        entry.custom_program = prog;
                    }
                }
            }
        }

        // Cache texture provider pointer (avoids scanning all entries every frame)
        auto* tp = interface_cast<ITextureProvider>(att);
        if (tp) {
            entry.texture_provider = tp;
        }
    }
}

void GlRenderer::rebuild_instances()
{
    rect_instances_.clear();
    text_instances_.clear();
    custom_shader_rects_.clear();

    for (auto& entry : entries_) {
        if (!entry.is_valid()) {
            continue;
        }

        auto state = velk::read_state<IElement>(entry.element);
        if (!state) {
            continue;
        }

        // Extract world position from world_matrix translation component
        float wx = state->world_matrix(0, 3);
        float wy = state->world_matrix(1, 3);

        for (auto& cmd : entry.cached_commands) {
            if (cmd.type == DrawCommandType::FillRect) {
                RectInstanceData inst;
                inst.x = wx + cmd.bounds.x;
                inst.y = wy + cmd.bounds.y;
                inst.width = cmd.bounds.width;
                inst.height = cmd.bounds.height;
                inst.color = cmd.color;

                if (entry.custom_program) {
                    custom_shader_rects_.push_back({inst, entry.custom_program});
                } else {
                    rect_instances_.push_back(inst);
                }
            } else if (cmd.type == DrawCommandType::TexturedQuad) {
                TextInstanceData inst;
                inst.x = wx + cmd.bounds.x;
                inst.y = wy + cmd.bounds.y;
                inst.width = cmd.bounds.width;
                inst.height = cmd.bounds.height;
                inst.color = cmd.color;
                inst.u0 = cmd.u0;
                inst.v0 = cmd.v0;
                inst.u1 = cmd.u1;
                inst.v1 = cmd.v1;
                text_instances_.push_back(inst);
            }
        }
    }
}

void GlRenderer::upload_atlas()
{
    atlas_dirty_ = false;

    for (auto& entry : entries_) {
        if (!entry.is_valid() || !entry.texture_provider) {
            continue;
        }

        auto* tp = entry.texture_provider;
        if (!tp->is_texture_dirty()) {
            continue;
        }

        uint32_t tw = tp->get_texture_width();
        uint32_t th = tp->get_texture_height();
        const uint8_t* pixels = tp->get_pixels();
        if (!pixels || tw == 0 || th == 0) {
            continue;
        }

        glBindTexture(GL_TEXTURE_2D, atlas_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RED,
                     static_cast<GLsizei>(tw),
                     static_cast<GLsizei>(th),
                     0,
                     GL_RED,
                     GL_UNSIGNED_BYTE,
                     pixels);
        glBindTexture(GL_TEXTURE_2D, 0);

        tp->clear_texture_dirty();
    }
}

IRenderer::VisualId GlRenderer::add_visual(const IElement::Ptr& element)
{
    if (!element) {
        return 0;
    }

    uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<uint32_t>(entries_.size());
        entries_.push_back({});
    }

    auto& entry = entries_[slot];
    entry.element = element;
    entry.alive = true;

    rebuild_commands(slot);
    instances_dirty_ = true;
    if (entry.texture_provider) {
        atlas_dirty_ = true;
    }

    object_to_slot_[element.get()] = slot;
    return slot + 1; // VisualId 0 = invalid
}

void GlRenderer::remove_visual(VisualId id)
{
    if (id == 0 || id > entries_.size()) {
        return;
    }

    uint32_t slot = id - 1;
    auto& entry = entries_[slot];
    if (!entry.alive) {
        return;
    }

    object_to_slot_.erase(entry.element.get());
    if (entry.custom_program) {
        glDeleteProgram(entry.custom_program);
        entry.custom_program = 0;
    }
    entry.element = nullptr;
    entry.cached_commands.clear();
    entry.texture_provider = nullptr;
    entry.alive = false;

    free_slots_.push_back(slot);
    instances_dirty_ = true;
}

void GlRenderer::update_visuals(velk::array_view<IElement*> changed)
{
    bool any_updated = false;
    for (auto* element : changed) {
        auto it = object_to_slot_.find(element);
        if (it == object_to_slot_.end()) {
            continue;
        }

        rebuild_commands(it->second);
        any_updated = true;

        if (entries_[it->second].texture_provider) {
            atlas_dirty_ = true;
        }
    }
    if (any_updated) {
        instances_dirty_ = true;
    }
}

} // namespace velk_ui
