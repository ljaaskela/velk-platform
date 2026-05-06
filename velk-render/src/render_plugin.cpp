#include "render_plugin.h"

#include "path/camera_pipeline.h"
#include "path/deferred_path.h"
#include "path/forward_path.h"
#include "path/post_process.h"
#include "frame/render_graph.h"

#include <velk-render/ext/default_render_pass.h>
#include "resource/render_texture_group.h"
#include "path/tonemap.h"
#include "material/material_property.h"
#include "mesh/mesh.h"
#include "mesh/mesh_buffer.h"
#include "mesh/mesh_builder.h"
#include "frame/frame_data_manager.h"
#include "frame/frame_snippet_registry.h"
#include "resource/gpu_resource_manager.h"
#include "resource/program_data_buffer.h"
#include <velk-render/ext/gpu_buffer.h>
#include "render_context.h"
#include "resource/render_texture.h"
#include "technique/rt_shadow.h"
#include "shader/shader.h"
#include "material/shader_material.h"
#include "material/standard_material.h"
#include "resource/surface.h"

#include <velk/ext/any.h>
#include <velk-render/render_types.h>

namespace velk {

ReturnValue RenderPlugin::initialize(IVelk& velk, PluginConfig& config)
{
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    auto rv = register_type<RenderContextImpl>(velk, alloc);
    rv &= register_type<GpuResourceManager>(velk, alloc);
    rv &= register_type<FrameDataManager>(velk, alloc);
    rv &= register_type<FrameSnippetRegistry>(velk, alloc);
    rv &= register_type<Shader>(velk);
    rv &= register_type<WindowSurface>(velk);
    rv &= register_type<RenderTexture>(velk);
    rv &= register_type<impl::RenderTextureGroup>(velk);
    rv &= register_type<impl::ShaderMaterial>(velk);
    rv &= register_type<impl::StandardMaterial>(velk);
    rv &= register_type<impl::BaseColorProperty>(velk);
    rv &= register_type<impl::MetallicRoughnessProperty>(velk);
    rv &= register_type<impl::NormalProperty>(velk);
    rv &= register_type<impl::OcclusionProperty>(velk);
    rv &= register_type<impl::EmissiveProperty>(velk);
    rv &= register_type<impl::SpecularProperty>(velk);
    rv &= register_type<impl::MaterialOptions>(velk);
    rv &= register_type<impl::RtShadow>(velk);
    rv &= register_type<impl::ProgramDataBuffer>(velk);
    rv &= register_type<impl::GpuBuffer>(velk);
    rv &= register_type<impl::Mesh>(velk);
    rv &= register_type<impl::MeshPrimitive>(velk);
    rv &= register_type<impl::MeshBuffer>(velk);
    rv &= register_type<impl::MeshBuilder>(velk);
    rv &= register_type<::velk::ext::AnyValue<UpdateRate>>(velk);

    // Built-in render paths (hive-allocated). RtPath ships in the
    // velk_rt sub-plugin so trivial UI apps don't pay for the RT
    // shader sources just by linking velk_render.
    rv &= register_type<ForwardPath>(velk);
    rv &= register_type<DeferredPath>(velk);

    // Default per-camera view pipeline. Auto-attached by Camera trait
    // ctor; must be registered before ScenePlugin's Camera registration
    // so the auto-attach create<>() succeeds. ScenePlugin lists
    // RenderPlugin as a dep so this ordering holds.
    rv &= register_type<impl::CameraPipeline>(velk);

    // Per-frame render graph. Renderer creates one per FrameSlot.
    rv &= register_type<impl::RenderGraph>(velk);
    rv &= register_type<impl::DefaultRenderPass>(velk);

    // Post-processing: container + first built-in effect.
    rv &= register_type<impl::PostProcess>(velk);
    rv &= register_type<impl::Tonemap>(velk);
    return rv;
}

ReturnValue RenderPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk
