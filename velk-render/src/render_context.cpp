#include "render_context.h"

#include "shader_compiler.h"
#include "spirv_material_reflect.h"
#include "surface.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-render/interface/intf_material_internal.h>
#include <velk-render/platform.h>

namespace velk {

namespace {

PipelineId compile_and_register(IRenderBackend& backend, string_view vert_glsl, string_view frag_glsl,
                                const ShaderIncludeMap* includes)
{
    auto vert_spirv = compile_glsl_to_spirv(vert_glsl, ShaderStage::Vertex, includes);
    auto frag_spirv = compile_glsl_to_spirv(frag_glsl, ShaderStage::Fragment, includes);
    if (vert_spirv.empty() || frag_spirv.empty()) {
        VELK_LOG(E, "compile_and_register: SPIR-V compilation failed");
        return 0;
    }

    PipelineDesc desc;
    desc.vertex_spirv = vert_spirv.data();
    desc.vertex_spirv_size = vert_spirv.size() * sizeof(uint32_t);
    desc.fragment_spirv = frag_spirv.data();
    desc.fragment_spirv_size = frag_spirv.size() * sizeof(uint32_t);

    return backend.create_pipeline(desc);
}

} // namespace

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

ISurface::Ptr RenderContextImpl::create_surface(int width, int height)
{
    auto obj = instance().create<IObject>(Surface::static_class_id());
    auto surface = interface_pointer_cast<ISurface>(obj);
    if (!surface) {
        return nullptr;
    }

    write_state<ISurface>(surface, [&](ISurface::State& s) {
        s.width = width;
        s.height = height;
    });

    return surface;
}

uint64_t RenderContextImpl::compile_pipeline(string_view fragment_source, string_view vertex_source,
                                             uint64_t key)
{
    if (!initialized_ || !backend_ || fragment_source.empty() || vertex_source.empty()) {
        return 0;
    }

    auto* includes = shader_includes_.empty() ? nullptr : &shader_includes_;
    PipelineId pid = compile_and_register(*backend_, vertex_source, fragment_source, includes);
    if (!pid) {
        return 0;
    }

    if (key == 0) {
        key = next_pipeline_key_++;
    }
    pipeline_map_[key] = pid;
    return key;
}

void RenderContextImpl::register_shader_include(string_view name, string_view content)
{
    shader_includes_[name] = content;
}

IMaterial::Ptr RenderContextImpl::create_shader_material(string_view fragment_source,
                                                         string_view vertex_source)
{
    if (vertex_source.empty()) {
        VELK_LOG(E, "create_shader_material: vertex_source is required");
        return nullptr;
    }
    uint64_t key = compile_pipeline(fragment_source, vertex_source);
    if (!key) {
        return nullptr;
    }

    auto mat = instance().create<IMaterialInternal>(ClassId::ShaderMaterial);
    if (!mat) {
        return nullptr;
    }

    mat->set_pipeline_handle(key);

    // Reflect material parameters from the vertex shader SPIR-V
    auto* includes = shader_includes_.empty() ? nullptr : &shader_includes_;
    auto vert_spirv = compile_glsl_to_spirv(vertex_source, ShaderStage::Vertex, includes);
    if (!vert_spirv.empty()) {
        auto params = reflect_material_params(vert_spirv.data(), vert_spirv.size());
        if (!params.empty()) {
            mat->setup_inputs(params);
        }
    }

    return mat;
}

} // namespace velk
