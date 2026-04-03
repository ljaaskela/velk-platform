#include "render_context.h"

#include "default_shaders.h"
#include "renderer.h"
#include "shader_compiler.h"
#include "spirv_reflect.h"
#include "surface.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-ui/interface/intf_material_internal.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

namespace {

bool compile_and_register(IRenderBackend& backend, uint64_t key,
                          const char* vert_glsl, const char* frag_glsl,
                          const VertexInputDesc& vertex_input,
                          ShaderTarget target)
{
    auto vert_spirv = compile_glsl_to_spirv(vert_glsl, ShaderStage::Vertex, target);
    auto frag_spirv = compile_glsl_to_spirv(frag_glsl, ShaderStage::Fragment, target);
    if (vert_spirv.empty() || frag_spirv.empty()) {
        VELK_LOG(E, "compile_and_register: SPIR-V compilation failed for pipeline %llu", key);
        return false;
    }

    // Reflect uniforms from SPIR-V
    auto vert_uniforms = reflect_spirv_uniforms(vert_spirv.data(), vert_spirv.size());
    auto frag_uniforms = reflect_spirv_uniforms(frag_spirv.data(), frag_spirv.size());

    PipelineDesc desc;
    desc.vertex_spirv = vert_spirv.data();
    desc.vertex_spirv_size = vert_spirv.size();
    desc.fragment_spirv = frag_spirv.data();
    desc.fragment_spirv_size = frag_spirv.size();
    desc.vertex_input = vertex_input;

    // Merge and deduplicate uniforms from both stages
    desc.uniforms = std::move(vert_uniforms);
    for (auto& u : frag_uniforms) {
        bool duplicate = false;
        for (auto& existing : desc.uniforms) {
            if (existing.name == u.name) { duplicate = true; break; }
        }
        if (!duplicate) {
            desc.uniforms.push_back(std::move(u));
        }
    }

    return backend.register_pipeline(key, desc);
}

} // namespace

bool RenderContextImpl::init(const RenderConfig& config)
{
    static constexpr velk::Uid kGlPluginId{"e1e9e004-21cd-4cfa-b843-49b0eb358149"};
    static constexpr velk::Uid kGlBackendId{"2302c979-1531-4d0b-bab6-d1bac99f0a11"};
    static constexpr velk::Uid kVkPluginId{"b91d4f6a-c583-47e0-9a1b-6e82d0f4a3b7"};
    static constexpr velk::Uid kVkBackendId{"f7a23c01-8e4d-4b19-a652-1d3f09b7e5c8"};

    velk::Uid plugin_id;
    velk::Uid class_id;

    switch (config.backend) {
    case RenderBackendType::GL:
        plugin_id = kGlPluginId;
        class_id = kGlBackendId;
        break;
    case RenderBackendType::Vulkan:
        plugin_id = kVkPluginId;
        class_id = kVkBackendId;
        break;
    default:
        VELK_LOG(E, "RenderContext::init: unsupported backend type %d", static_cast<int>(config.backend));
        return false;
    }

    auto& reg = velk::instance().plugin_registry();
    if (!reg.get_or_load_plugin(plugin_id)) {
        VELK_LOG(E, "RenderContext::init: failed to load backend plugin");
        return false;
    }

    auto obj = velk::instance().create<velk::IObject>(class_id);
    backend_ = interface_pointer_cast<IRenderBackend>(obj);
    if (!backend_) {
        VELK_LOG(E, "RenderContext::init: failed to create backend");
        return false;
    }

    if (!backend_->init(config.backend_params)) {
        VELK_LOG(E, "RenderContext::init: backend init failed");
        backend_ = nullptr;
        return false;
    }

    backend_type_ = config.backend;
    auto shader_target = (config.backend == RenderBackendType::Vulkan)
        ? ShaderTarget::Vulkan : ShaderTarget::OpenGL;

    // Untextured: {x, y, w, h} at loc 0, {r, g, b, a} at loc 1 = 32 bytes
    VertexInputDesc untextured_input;
    untextured_input.stride = 32;
    untextured_input.attributes = {
        {0, 0,  VertexAttribType::Float4},  // inst_rect
        {1, 16, VertexAttribType::Float4},  // inst_color
    };

    // Textured: untextured + {u0, v0, u1, v1} at loc 2 = 48 bytes
    VertexInputDesc textured_input;
    textured_input.stride = 48;
    textured_input.attributes = {
        {0, 0,  VertexAttribType::Float4},  // inst_rect
        {1, 16, VertexAttribType::Float4},  // inst_color
        {2, 32, VertexAttribType::Float4},  // inst_uv
    };

    // Compile built-in GLSL shaders to SPIR-V and register pipelines
    compile_and_register(*backend_, PipelineKey::Rect,
                         rect_vertex_src, rect_fragment_src, untextured_input, shader_target);
    compile_and_register(*backend_, PipelineKey::Text,
                         text_vertex_src, text_fragment_src, textured_input, shader_target);
    compile_and_register(*backend_, PipelineKey::RoundedRect,
                         rounded_rect_vertex_src, rounded_rect_fragment_src, untextured_input, shader_target);

    initialized_ = true;
    VELK_LOG(I, "RenderContext initialized (backend=%d)", static_cast<int>(config.backend));
    return true;
}

ISurface::Ptr RenderContextImpl::create_surface(int width, int height)
{
    auto obj = velk::instance().create<velk::IObject>(Surface::static_class_id());
    auto surface = interface_pointer_cast<ISurface>(obj);
    if (!surface) {
        return nullptr;
    }

    velk::write_state<ISurface>(surface, [&](ISurface::State& s) {
        s.width = width;
        s.height = height;
    });

    if (backend_) {
        uint64_t sid = next_surface_id_++;
        SurfaceDesc desc{width, height};
        backend_->create_surface(sid, desc);
    }

    return surface;
}

IRenderer::Ptr RenderContextImpl::create_renderer()
{
    if (!initialized_ || !backend_) {
        VELK_LOG(E, "RenderContext::create_renderer: context not initialized");
        return nullptr;
    }

    auto obj = velk::instance().create<velk::IObject>(ClassId::Renderer);
    if (!obj) {
        VELK_LOG(E, "RenderContext::create_renderer: failed to create renderer");
        return nullptr;
    }

    auto* internal = interface_cast<IRendererInternal>(obj);
    if (internal) {
        internal->set_backend(backend_, this);
    }

    return interface_pointer_cast<IRenderer>(obj);
}

velk::IObject::Ptr RenderContextImpl::create_shader_material(const char* fragment_source,
                                                              const char* vertex_source)
{
    if (!initialized_ || !backend_ || !fragment_source) {
        return nullptr;
    }

    // Default untextured vertex input for custom materials
    VertexInputDesc untextured_input;
    untextured_input.stride = 32;
    untextured_input.attributes = {
        {0, 0,  VertexAttribType::Float4},
        {1, 16, VertexAttribType::Float4},
    };

    uint64_t key = next_pipeline_key_++;
    auto shader_target = (backend_type_ == RenderBackendType::Vulkan)
        ? ShaderTarget::Vulkan : ShaderTarget::OpenGL;
    const char* vert_src = vertex_source ? vertex_source : rect_vertex_src;
    if (!compile_and_register(*backend_, key, vert_src, fragment_source, untextured_input, shader_target)) {
        return nullptr;
    }

    auto obj = velk::instance().create<velk::IObject>(ClassId::Material::Shader);
    if (!obj) {
        return nullptr;
    }

    auto* internal = interface_cast<IMaterialInternal>(obj);
    if (internal) {
        internal->set_pipeline_handle(key);
    }

    return obj;
}

} // namespace velk_ui
