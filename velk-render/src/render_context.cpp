#include "render_context.h"

#include "default_shaders.h"
#include "shader_compiler.h"
#include "surface.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_material_internal.h>
#include <velk-render/platform.h>

namespace velk {

namespace {

PipelineId compile_and_register(IRenderBackend& backend,
                                const char* vert_glsl, const char* frag_glsl)
{
    auto vert_spirv = compile_glsl_to_spirv(vert_glsl, ShaderStage::Vertex);
    auto frag_spirv = compile_glsl_to_spirv(frag_glsl, ShaderStage::Fragment);
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

    // Compile and register built-in pipelines
    auto rect_id = compile_and_register(*backend_, rect_vertex_src, rect_fragment_src);
    auto text_id = compile_and_register(*backend_, text_vertex_src, text_fragment_src);
    auto rounded_rect_id = compile_and_register(*backend_, rounded_rect_vertex_src, rounded_rect_fragment_src);

    if (!rect_id || !text_id || !rounded_rect_id) {
        VELK_LOG(E, "RenderContext::init: failed to compile built-in shaders");
        backend_ = nullptr;
        return false;
    }

    pipeline_map_[PipelineKey::Rect] = rect_id;
    pipeline_map_[PipelineKey::Text] = text_id;
    pipeline_map_[PipelineKey::RoundedRect] = rounded_rect_id;

    initialized_ = true;
    VELK_LOG(I, "RenderContext initialized (Vulkan, pointer-based)");
    return true;
}

ISurface::Ptr RenderContextImpl::create_surface(int width, int height)
{
    auto obj = instance().create<IObject>(Surface::static_class_id());
    auto surface = interface_pointer_cast<ISurface>(obj);
    if (!surface) return nullptr;

    write_state<ISurface>(surface, [&](ISurface::State& s) {
        s.width = width;
        s.height = height;
    });

    return surface;
}

IObject::Ptr RenderContextImpl::create_shader_material(const char* fragment_source,
                                                        const char* vertex_source)
{
    if (!initialized_ || !backend_ || !fragment_source) return nullptr;

    const char* vert_src = vertex_source ? vertex_source : rect_vertex_src;
    PipelineId pid = compile_and_register(*backend_, vert_src, fragment_source);
    if (!pid) return nullptr;

    uint64_t key = next_pipeline_key_++;
    pipeline_map_[key] = pid;

    auto obj = instance().create<IObject>(ClassId::ShaderMaterial);
    if (!obj) return nullptr;

    auto* mat_internal = interface_cast<IMaterialInternal>(obj);
    if (mat_internal) {
        mat_internal->set_pipeline_handle(key);
    }

    return obj;
}

} // namespace velk
