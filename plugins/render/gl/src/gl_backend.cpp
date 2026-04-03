#include "gl_backend.h"

#include <cstring>
#include <glad/gl.h>

namespace velk_ui {

namespace {

GLuint load_spirv_shader(GLenum type, const uint32_t* spirv, size_t word_count)
{
    GLuint shader = glCreateShader(type);
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V,
                   spirv, static_cast<GLsizei>(word_count * sizeof(uint32_t)));
    glSpecializeShader(shader, "main", 0, nullptr, nullptr);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        VELK_LOG(E, "SPIR-V specialization error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint build_program(const uint32_t* vert_spirv, size_t vert_size,
                     const uint32_t* frag_spirv, size_t frag_size)
{
    GLuint vert = load_spirv_shader(GL_VERTEX_SHADER, vert_spirv, vert_size);
    GLuint frag = load_spirv_shader(GL_FRAGMENT_SHADER, frag_spirv, frag_size);
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return 0;
    }

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
        program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
    return program;
}

GLint attrib_component_count(VertexAttribType type)
{
    switch (type) {
    case VertexAttribType::Float:  return 1;
    case VertexAttribType::Float2: return 2;
    case VertexAttribType::Float3: return 3;
    case VertexAttribType::Float4: return 4;
    }
    return 4;
}

void setup_vao(GLuint vao, GLuint vbo, const VertexInputDesc& desc)
{
    glBindVertexArray(vao);

    for (auto& attr : desc.attributes) {
        glEnableVertexAttribArray(attr.location);
        glVertexAttribFormat(attr.location,
                             attrib_component_count(attr.type),
                             GL_FLOAT, GL_FALSE, attr.offset);
        glVertexAttribBinding(attr.location, 0);
    }

    glBindVertexBuffer(0, vbo, 0, desc.stride);
    glVertexBindingDivisor(0, 1);

    glBindVertexArray(0);
}

} // namespace

GlBackend::~GlBackend()
{
    if (initialized_) {
        GlBackend::shutdown();
    }
}

bool GlBackend::init(void* params)
{
    if (params) {
        auto loader = reinterpret_cast<GLADloadfunc>(params);
        if (!gladLoadGL(loader)) {
            VELK_LOG(E, "GlBackend::init: failed to load GL functions");
            return false;
        }
        VELK_LOG(I, "OpenGL %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Create globals UBO (binding 0)
    glCreateBuffers(1, &globals_ubo_);
    glNamedBufferStorage(globals_ubo_, kGlobalsUboSize, nullptr, GL_DYNAMIC_STORAGE_BIT);

    // Create material UBO (binding 1)
    glCreateBuffers(1, &material_ubo_);
    glNamedBufferStorage(material_ubo_, kMaterialUboMaxSize, nullptr, GL_DYNAMIC_STORAGE_BIT);

    initialized_ = true;
    return true;
}

void GlBackend::shutdown()
{
    if (!initialized_) return;

    for (auto& [key, entry] : pipelines_) {
        if (entry.program) glDeleteProgram(entry.program);
        if (entry.vbo) glDeleteBuffers(1, &entry.vbo);
        if (entry.vao) glDeleteVertexArrays(1, &entry.vao);
    }
    pipelines_.clear();

    for (auto& [key, tex] : textures_) {
        if (tex) glDeleteTextures(1, &tex);
    }
    textures_.clear();

    if (globals_ubo_) { glDeleteBuffers(1, &globals_ubo_); globals_ubo_ = 0; }
    if (material_ubo_) { glDeleteBuffers(1, &material_ubo_); material_ubo_ = 0; }

    surfaces_.clear();
    initialized_ = false;
}

bool GlBackend::create_surface(uint64_t surface_id, const SurfaceDesc& desc)
{
    surfaces_[surface_id] = {desc.width, desc.height};
    return true;
}

void GlBackend::destroy_surface(uint64_t surface_id)
{
    surfaces_.erase(surface_id);
}

void GlBackend::update_surface(uint64_t surface_id, const SurfaceDesc& desc)
{
    auto it = surfaces_.find(surface_id);
    if (it != surfaces_.end()) {
        it->second = {desc.width, desc.height};
    }
}

bool GlBackend::register_pipeline(uint64_t pipeline_key, const PipelineDesc& desc)
{
    if (pipelines_.count(pipeline_key)) return true;

    GLuint program = build_program(desc.vertex_spirv, desc.vertex_spirv_size,
                                   desc.fragment_spirv, desc.fragment_spirv_size);
    if (!program) return false;

    PipelineEntry entry;
    entry.program = program;

    glGenVertexArrays(1, &entry.vao);
    glGenBuffers(1, &entry.vbo);
    setup_vao(entry.vao, entry.vbo, desc.vertex_input);

    entry.uniforms = desc.uniforms;

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        VELK_LOG(E, "GL error 0x%X during pipeline %llu registration", err, pipeline_key);
    }

    pipelines_[pipeline_key] = std::move(entry);
    return true;
}

velk::vector<UniformInfo> GlBackend::get_pipeline_uniforms(uint64_t pipeline_key) const
{
    auto it = pipelines_.find(pipeline_key);
    if (it != pipelines_.end()) return it->second.uniforms;
    return {};
}

void GlBackend::upload_texture(uint64_t texture_key,
                               const uint8_t* pixels, int width, int height)
{
    GLuint tex = 0;
    auto it = textures_.find(texture_key);
    if (it != textures_.end()) {
        tex = it->second;
    } else {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        textures_[texture_key] = tex;
    }

    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GlBackend::begin_frame(uint64_t surface_id)
{
    current_surface_ = surface_id;

    auto it = surfaces_.find(surface_id);
    if (it == surfaces_.end()) {
        VELK_LOG(E, "GlBackend::begin_frame: unknown surface %llu", surface_id);
        return;
    }

    int w = it->second.width;
    int h = it->second.height;

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float L = 0.0f, R = static_cast<float>(w);
    float T = 0.0f, B = static_cast<float>(h);

    std::memset(projection_, 0, sizeof(projection_));
    projection_[0]  = 2.0f / (R - L);
    projection_[5]  = 2.0f / (T - B);
    projection_[10] = -1.0f;
    projection_[12] = -(R + L) / (R - L);
    projection_[13] = -(T + B) / (T - B);
    projection_[15] = 1.0f;
}

void GlBackend::submit(velk::array_view<const RenderBatch> batches)
{
    for (auto& batch : batches) {
        if (batch.instance_count == 0) continue;

        auto pit = pipelines_.find(batch.pipeline_key);
        if (pit == pipelines_.end()) continue;
        auto& pipeline = pit->second;

        glUseProgram(pipeline.program);

        // Pack and upload globals UBO (binding 0): projection (64B) + rect (16B)
        struct {
            float projection[16];
            float rect[4];
        } globals;
        std::memcpy(globals.projection, projection_, sizeof(projection_));
        if (batch.has_rect) {
            globals.rect[0] = batch.rect.x;
            globals.rect[1] = batch.rect.y;
            globals.rect[2] = batch.rect.width;
            globals.rect[3] = batch.rect.height;
        } else {
            std::memset(globals.rect, 0, sizeof(globals.rect));
        }
        glNamedBufferSubData(globals_ubo_, 0, sizeof(globals), &globals);
        glBindBufferBase(GL_UNIFORM_BUFFER, kGlobalsUboBinding, globals_ubo_);

        // Pack and upload material UBO (binding 1) if there are material uniforms
        if (!batch.uniforms.empty()) {
            uint8_t material_data[kMaterialUboMaxSize] = {};
            size_t max_offset = 0;
            for (auto& u : batch.uniforms) {
                if (u.location < 0) continue;
                size_t offset = static_cast<size_t>(u.location);
                size_t data_size = 0;
                if (u.typeUid == velk::type_uid<float>()) data_size = 4;
                else if (u.typeUid == velk::type_uid<velk::color>() || u.typeUid == velk::type_uid<velk::vec4>()) data_size = 16;
                else if (u.typeUid == velk::type_uid<velk::mat4>()) data_size = 64;
                else if (u.typeUid == velk::type_uid<int32_t>()) data_size = 4;
                else if (u.typeUid == velk::type_uid<velk::vec2>()) data_size = 8;

                if (data_size > 0 && offset + data_size <= kMaterialUboMaxSize) {
                    std::memcpy(material_data + offset, u.data, data_size);
                    if (offset + data_size > max_offset) max_offset = offset + data_size;
                }
            }
            if (max_offset > 0) {
                glNamedBufferSubData(material_ubo_, 0, static_cast<GLsizeiptr>(max_offset), material_data);
                glBindBufferBase(GL_UNIFORM_BUFFER, kMaterialUboBinding, material_ubo_);
            }
        }

        // Bind texture
        if (batch.texture_key != 0) {
            auto tit = textures_.find(batch.texture_key);
            if (tit != textures_.end()) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tit->second);
            }
        }

        // Upload instance data and draw
        glNamedBufferData(pipeline.vbo,
                          static_cast<GLsizeiptr>(batch.instance_data.size()),
                          batch.instance_data.data(), GL_STREAM_DRAW);
        glBindVertexArray(pipeline.vao);
        glBindVertexBuffer(0, pipeline.vbo, 0, batch.instance_stride);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                              static_cast<GLsizei>(batch.instance_count));

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            VELK_LOG(E, "GL error 0x%X after draw (pipeline %llu, %u instances)",
                     err, batch.pipeline_key, batch.instance_count);
        }

        glBindVertexArray(0);

        if (batch.texture_key != 0) {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
}

void GlBackend::end_frame()
{
    current_surface_ = 0;
}

} // namespace velk_ui
