#include "render_context.h"

#include "shader/shader.h"
#include "shader/shader_compiler.h"
#include "material/spirv_material_reflect.h"
#include "resource/surface.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/hash.h>

#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/frame/raster_shaders.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_surface.h>
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

    // Default UV1 buffer: one vec2(0, 0) shared by every draw whose
    // primitive has no TEXCOORD_1. The Renderer uploads it once at
    // set_backend time; `velk_uv1` reads index 0 via
    // DrawDataHeader::uv1_enabled == 0.
    {
        const float zero_uv1[2] = {0.f, 0.f};
        default_uv1_ = mesh_builder_->build_buffer(zero_uv1, sizeof(zero_uv1), nullptr, 0);
        if (!default_uv1_) {
            VELK_LOG(E, "RenderContext::init: failed to create default uv1 buffer");
            backend_ = nullptr;
            return false;
        }
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
        s.color_format = config.color_format;
    });

    surface->set_depth_format(config.depth);

    // Eagerly create the backend-side swapchain so the backend's default
    // render pass gets upgraded to match the surface's depth config
    // before any pipelines are compiled. Otherwise a pipeline compiled
    // between create_surface() and add_view() would target the initial
    // depth-less default render pass and be incompatible with the
    // swapchain's depth-enabled render pass at draw time.
    if (backend_) {
        SurfaceDesc desc{};
        desc.width = config.width;
        desc.height = config.height;
        desc.color_format = config.color_format;
        desc.update_rate = config.update_rate;
        desc.target_fps = config.target_fps;
        desc.depth = config.depth;
        uint64_t sid = backend_->create_surface(desc);
        if (sid != 0) {
            surface->set_gpu_handle(GpuResourceKey::Default, sid);
        }
    }

    return surface;
}

IMeshBuilder& RenderContextImpl::get_mesh_builder()
{
    return *mesh_builder_;
}

IBuffer::Ptr RenderContextImpl::get_default_buffer(DefaultBufferType type) const
{
    switch (type) {
    case DefaultBufferType::Uv1:
        return interface_pointer_cast<IBuffer>(default_uv1_);
    }
    return nullptr;
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

IGpuPipeline::Ptr RenderContextImpl::compile_pipeline_dynamic(
    string_view fragment_source, string_view vertex_source,
    uint64_t key, array_view<const PixelFormat> color_formats,
    DepthFormat depth_format, const PipelineOptions& options,
    uint64_t* out_key)
{
    if (!initialized_ || !backend_) {
        return {};
    }
    auto vert_src = vertex_source.empty() ? nullptr
                  : compile_shader(vertex_source, ShaderStage::Vertex);
    auto frag_src = fragment_source.empty() ? nullptr
                  : compile_shader(fragment_source, ShaderStage::Fragment);
    const auto& vert_shader = vert_src ? vert_src : default_vertex_shader_;
    const auto& frag_shader = frag_src ? frag_src : default_fragment_shader_;
    if (!vert_shader || !frag_shader) {
        VELK_LOG(E, "compile_pipeline_dynamic: missing vertex or fragment shader");
        return {};
    }

    PipelineDesc desc;
    desc.vertex = vert_shader;
    desc.fragment = frag_shader;
    desc.options = options;

    auto pid = backend_->create_pipeline_dynamic(desc, color_formats, depth_format);
    if (!pid) return {};

    if (key == 0) {
        key = next_pipeline_key_++;
    }
    // Cache slot is `(user_key, color_formats[0], layout)`. The layout
    // signature (derived from the full color-format set) differentiates MRT
    // pipelines (e.g. gbuffer) from single-color forward ones sharing the
    // same user_key, without keying on a render-target instance. The cache
    // holds only a weak ref; the returned strong Ptr is the caller's to keep.
    PixelFormat cache_format = color_formats.empty()
        ? PixelFormat::RGBA8
        : color_formats[0];
    pipeline_map_[PipelineCacheKey{key, cache_format, depth_format,
                                   pipeline_target_layout(color_formats)}] = pid;
    if (out_key) *out_key = key;
    return pid;
}

IGpuPipeline::Ptr RenderContextImpl::create_compute_pipeline(const IShader::Ptr& compute, uint64_t key)
{
    if (!initialized_ || !backend_ || !compute) {
        return {};
    }

    ComputePipelineDesc desc;
    desc.compute = compute;

    auto pid = backend_->create_compute_pipeline(desc);
    if (!pid) {
        return {};
    }

    if (key == 0) {
        key = next_pipeline_key_++;
    }
    // Compute pipelines are render-pass independent; key under a
    // canonical (RGBA8, layout 0) placeholder tuple so call sites look
    // them up with just the user_key. Cache holds a weak ref; the returned
    // strong Ptr is the caller's to keep.
    pipeline_map_[PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0}] = pid;
    return pid;
}

IGpuPipeline::Ptr RenderContextImpl::compile_compute_pipeline(string_view compute_source, uint64_t key)
{
    if (compute_source.empty()) {
        return {};
    }
    auto compute = compile_shader(compute_source, ShaderStage::Compute);
    if (!compute) {
        return {};
    }
    return create_compute_pipeline(compute, key);
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
    auto mat = instance().create<IMaterialInternal>(ClassId::ShaderMaterial);
    if (!mat) {
        return nullptr;
    }

    // Hand over the sources; pipeline compilation is the renderer's job
    // and happens lazily on first draw — so any IMaterialOptions attached
    // between now and then is honored, same as every other material.
    mat->set_sources(vertex_source, fragment_source);

    // Reflect material parameters from the vertex shader SPIR-V for
    // dynamic-property setup. Compilation hits the shader cache on
    // second run (inside the renderer's pipeline compile).
    auto vert = vertex_source.empty()
                    ? default_vertex_shader_
                    : compile_shader(vertex_source, ShaderStage::Vertex);
    if (vert) {
        auto vert_data = vert->get_data();
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
