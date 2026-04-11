#include "render_context.h"

#include "shader.h"
#include "shader_compiler.h"
#include "spirv_material_reflect.h"
#include "surface.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-render/interface/intf_material_internal.h>
#include <velk-render/platform.h>

namespace velk {

bool RenderContextImpl::init(const RenderConfig& config)
{
    RenderBackendType backend_type = config.backend;
    if (backend_type == RenderBackendType::Default) {
        backend_type = RenderBackendType::Vulkan;
    }

    Uid plugin_id;
    Uid class_id;

    switch (backend_type) {
    case RenderBackendType::Vulkan:
        plugin_id = PluginId::VkPlugin;
        class_id = ClassId::VkBackend;
        break;
    default:
        VELK_LOG(E, "RenderContext::init: unsupported backend type %d", static_cast<int>(backend_type));
        return false;
    }

    auto& reg = instance().plugin_registry();
    if (!reg.get_or_load_plugin(plugin_id)) {
        VELK_LOG(E, "RenderContext::init: failed to load backend plugin");
        return false;
    }

    auto obj = instance().create<IObject>(class_id);
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

    initialized_ = true;
    VELK_LOG(I, "RenderContext initialized (Vulkan, pointer-based)");
    return true;
}

ISurface::Ptr RenderContextImpl::create_surface(const SurfaceConfig& config)
{
    auto obj = instance().create<IObject>(Surface::static_class_id());
    auto surface = interface_pointer_cast<ISurface>(obj);
    if (!surface) {
        return nullptr;
    }

    write_state<ISurface>(surface, [&](ISurface::State& s) {
        s.width = config.width;
        s.height = config.height;
        s.update_rate = config.update_rate;
        s.target_fps = config.target_fps;
    });

    return surface;
}

IShader::Ptr RenderContextImpl::compile_shader(string_view source, ShaderStage stage)
{
    if (!initialized_ || source.empty()) {
        return nullptr;
    }

    auto* includes = shader_includes_.empty() ? nullptr : &shader_includes_;
    auto spirv = compile_glsl_to_spirv(source, stage, includes);
    if (spirv.empty()) {
        return nullptr;
    }

    auto shader = instance().create<IShader>(Shader::static_class_id());
    if (!shader) {
        return nullptr;
    }
    shader->init(std::move(spirv));
    return shader;
}

uint64_t RenderContextImpl::create_pipeline(const IShader::Ptr& vertex, const IShader::Ptr& fragment,
                                            uint64_t key)
{
    if (!initialized_ || !backend_) {
        return 0;
    }

    const auto& vert_shader = vertex ? vertex : default_vertex_shader_;
    const auto& frag_shader = fragment ? fragment : default_fragment_shader_;
    if (!vert_shader || !frag_shader) {
        VELK_LOG(E, "create_pipeline: missing vertex or fragment shader");
        return 0;
    }

    auto vert_data = vert_shader->get_data();
    auto frag_data = frag_shader->get_data();
    if (vert_data.empty() || frag_data.empty()) {
        VELK_LOG(E, "create_pipeline: empty shader bytecode");
        return 0;
    }

    PipelineDesc desc;
    /*desc.vertex_spirv = vert_data.begin();
    desc.vertex_spirv_size = vert_data.size() * sizeof(uint32_t);
    desc.fragment_spirv = frag_data.begin();
    desc.fragment_spirv_size = frag_data.size() * sizeof(uint32_t);*/
    desc.vertex = vert_shader;
    desc.fragment = frag_shader;

    PipelineId pid = backend_->create_pipeline(desc);
    if (!pid) {
        return 0;
    }

    if (key == 0) {
        key = next_pipeline_key_++;
    }
    pipeline_map_[key] = pid;
    return key;
}

uint64_t RenderContextImpl::compile_pipeline(string_view fragment_source, string_view vertex_source,
                                             uint64_t key)
{
    auto vert = vertex_source.empty() ? nullptr : compile_shader(vertex_source, ShaderStage::Vertex);
    auto frag = fragment_source.empty() ? nullptr : compile_shader(fragment_source, ShaderStage::Fragment);
    return create_pipeline(vert, frag, key);
}

void RenderContextImpl::set_default_vertex_shader(const IShader::Ptr& shader)
{
    default_vertex_shader_ = shader;
}

void RenderContextImpl::set_default_fragment_shader(const IShader::Ptr& shader)
{
    default_fragment_shader_ = shader;
}

void RenderContextImpl::register_shader_include(string_view name, string_view content)
{
    shader_includes_[name] = content;
}

IMaterial::Ptr RenderContextImpl::create_shader_material(string_view fragment_source,
                                                         string_view vertex_source)
{
    auto vert = vertex_source.empty() ? nullptr : compile_shader(vertex_source, ShaderStage::Vertex);
    auto frag = fragment_source.empty() ? nullptr : compile_shader(fragment_source, ShaderStage::Fragment);

    uint64_t key = create_pipeline(vert, frag);
    if (!key) {
        return nullptr;
    }

    auto mat = instance().create<IMaterialInternal>(ClassId::ShaderMaterial);
    if (!mat) {
        return nullptr;
    }

    mat->set_pipeline_handle(key);

    // Reflect material parameters from the vertex shader SPIR-V
    const auto& vert_shader = vert ? vert : default_vertex_shader_;
    if (vert_shader) {
        auto vert_data = vert_shader->get_data();
        if (!vert_data.empty()) {
            auto params = reflect_material_params(vert_data.begin(), vert_data.size());
            if (!params.empty()) {
                mat->setup_inputs(params);
            }
        }
    }

    return mat;
}

} // namespace velk
