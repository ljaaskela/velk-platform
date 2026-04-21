#include "render_context.h"

#include "shader.h"
#include "shader_compiler.h"
#include "spirv_material_reflect.h"
#include "surface.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/hash.h>

#include <velk-render/interface/material/intf_material_internal.h>
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

    // Register the framework-level velk.glsl include so it appears in
    // shader_includes_ alongside any plugin-registered includes. The shader
    // cache uses this map to compute its per-shader cache keys.
    shader_includes_["velk.glsl"] = string(kVelkGlsl);

    mesh_builder_ = instance().create<IMeshBuilder>(ClassId::MeshBuilder);
    if (!mesh_builder_) {
        VELK_LOG(E, "RenderContext::init: failed to create mesh builder");
        backend_ = nullptr;
        return false;
    }

    initialized_ = true;
    VELK_LOG(I, "RenderContext initialized (Vulkan, bindless)");
    return true;
}

IWindowSurface::Ptr RenderContextImpl::create_surface(const SurfaceConfig& config)
{
    auto obj = instance().create<IObject>(WindowSurface::static_class_id());
    auto surface = interface_pointer_cast<IWindowSurface>(obj);
    if (!surface) {
        return nullptr;
    }

    write_state<IWindowSurface>(surface, [&](IWindowSurface::State& s) {
        s.size = {static_cast<uint32_t>(config.width), static_cast<uint32_t>(config.height)};
        s.update_rate = config.update_rate;
        s.target_fps = config.target_fps;
    });

    return surface;
}

IMeshBuilder& RenderContextImpl::get_mesh_builder()
{
    return *mesh_builder_;
}

IShader::Ptr RenderContextImpl::compile_shader(string_view source, ShaderStage stage, uint64_t key)
{
    if (!initialized_ || source.empty()) {
        return nullptr;
    }

    if (key == 0) {
        key = make_hash64(source);
    }

    shader_cache_.ensure_initialized();

    // Combined cache key: source key XOR stage discriminator XOR hash of all
    // currently-registered includes. Folding the include hash into the key
    // means that any change to a virtual include (e.g. velk.glsl) naturally
    // invalidates affected entries; old entries with the previous include
    // content become orphans rather than corrupt cache hits.
    constexpr uint64_t kStageVertexMix = 0x68f3df8b8e0c8b8dULL;
    constexpr uint64_t kStageFragmentMix = 0xa24baed4963ee407ULL;
    constexpr uint64_t kStageComputeMix = 0x5a3b1d2f6e9c8411ULL;
    uint64_t stage_mix = kStageFragmentMix;
    switch (stage) {
    case ShaderStage::Vertex:   stage_mix = kStageVertexMix;   break;
    case ShaderStage::Fragment: stage_mix = kStageFragmentMix; break;
    case ShaderStage::Compute:  stage_mix = kStageComputeMix;  break;
    }
    uint64_t include_hash = hash_shader_includes(shader_includes_);
    uint64_t cache_key = key ^ stage_mix ^ include_hash;

    auto cached = shader_cache_.read(cache_key);
    if (!cached.empty()) {
        auto shader = instance().create<IShader>(Shader::static_class_id());
        if (shader) {
            shader->init(std::move(cached));
            return shader;
        }
    }

    auto* includes = shader_includes_.empty() ? nullptr : &shader_includes_;
    auto spirv = compile_glsl_to_spirv(source, stage, includes);
    if (spirv.empty()) {
        return nullptr;
    }

    shader_cache_.write(cache_key, spirv);

    auto shader = instance().create<IShader>(Shader::static_class_id());
    if (!shader) {
        return nullptr;
    }
    shader->init(std::move(spirv));
    return shader;
}

uint64_t RenderContextImpl::create_pipeline(const IShader::Ptr& vertex, const IShader::Ptr& fragment,
                                            uint64_t key, RenderTargetGroup target_group,
                                            CullMode cull_mode, BlendMode blend_mode,
                                            Topology topology)
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
    desc.vertex = vert_shader;
    desc.fragment = frag_shader;
    desc.cull_mode = cull_mode;
    desc.blend_mode = blend_mode;
    desc.topology = topology;

    PipelineId pid = backend_->create_pipeline(desc, target_group);
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
                                             uint64_t key, RenderTargetGroup target_group,
                                             CullMode cull_mode, BlendMode blend_mode,
                                             Topology topology)
{
    auto vert = vertex_source.empty() ? nullptr : compile_shader(vertex_source, ShaderStage::Vertex);
    auto frag = fragment_source.empty() ? nullptr : compile_shader(fragment_source, ShaderStage::Fragment);
    return create_pipeline(vert, frag, key, target_group, cull_mode, blend_mode, topology);
}

uint64_t RenderContextImpl::create_compute_pipeline(const IShader::Ptr& compute, uint64_t key)
{
    if (!initialized_ || !backend_ || !compute) {
        return 0;
    }

    ComputePipelineDesc desc;
    desc.compute = compute;

    PipelineId pid = backend_->create_compute_pipeline(desc);
    if (!pid) {
        return 0;
    }

    if (key == 0) {
        key = next_pipeline_key_++;
    }
    pipeline_map_[key] = pid;
    return key;
}

uint64_t RenderContextImpl::compile_compute_pipeline(string_view compute_source, uint64_t key)
{
    if (compute_source.empty()) {
        return 0;
    }
    auto compute = compile_shader(compute_source, ShaderStage::Compute);
    if (!compute) {
        return 0;
    }
    return create_compute_pipeline(compute, key);
}

PipelineId RenderContextImpl::compile_gbuffer_pipeline(string_view fragment_source,
                                                       string_view vertex_source,
                                                       uint64_t key,
                                                       RenderTargetGroup target_group,
                                                       CullMode cull_mode,
                                                       Topology topology)
{
    if (!initialized_ || !backend_ || key == 0 || target_group == 0) {
        return 0;
    }
    auto it = gbuffer_pipeline_map_.find(key);
    if (it != gbuffer_pipeline_map_.end()) {
        return it->second;
    }

    auto vert = vertex_source.empty()
                    ? default_gbuffer_vertex_shader_
                    : compile_shader(vertex_source, ShaderStage::Vertex);
    auto frag = fragment_source.empty()
                    ? default_gbuffer_fragment_shader_
                    : compile_shader(fragment_source, ShaderStage::Fragment);
    if (!vert || !frag) {
        VELK_LOG(E, "compile_gbuffer_pipeline: missing vertex or fragment shader for key %llu",
                 static_cast<unsigned long long>(key));
        return 0;
    }

    PipelineDesc desc;
    desc.vertex = vert;
    desc.fragment = frag;
    desc.cull_mode = cull_mode;
    // G-buffer passes always write opaquely regardless of alpha mode.
    desc.blend_mode = BlendMode::Opaque;
    desc.topology = topology;

    PipelineId pid = backend_->create_pipeline(desc, target_group);
    if (!pid) {
        return 0;
    }
    gbuffer_pipeline_map_[key] = pid;
    return pid;
}

void RenderContextImpl::set_default_gbuffer_vertex_shader(const IShader::Ptr& shader)
{
    default_gbuffer_vertex_shader_ = shader;
}

void RenderContextImpl::set_default_gbuffer_fragment_shader(const IShader::Ptr& shader)
{
    default_gbuffer_fragment_shader_ = shader;
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
