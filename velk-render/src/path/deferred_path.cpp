#include "path/deferred_path.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

#include <velk-render/frame/compute_shaders.h>
#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/frame/raster_shaders.h>
#include "path/deferred_gbuffer.h"
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/material/intf_material.h>

#include <velk/interface/intf_object.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velk {

namespace {

/// Resolves (or lazy-compiles) the deferred g-buffer pipeline for a
/// batch. The cache key is the forward key XOR'd with a per-visual-
/// class perturbation derived from the shader-source class uid; that
/// way two visuals sharing a material still get distinct pipelines
/// with the right `velk_visual_discard` body. Returns 0 to skip.
PipelineId resolve_or_compile_gbuffer(IRenderContext& ctx,
                                      const IBatch& batch,
                                      RenderTargetGroup target_group)
{
    auto material_ptr = batch.material();
    auto shader_source_ptr = batch.shader_source();
    uint64_t forward_key = batch.pipeline_key();
    if (forward_key == 0 && material_ptr) {
        forward_key = material_ptr->get_pipeline_handle(ctx);
    }
    if (forward_key == 0) return 0;

    uint64_t perturb = 0;
    if (auto* obj = interface_cast<IObject>(shader_source_ptr.get())) {
        Uid uid = obj->get_class_uid();
        perturb = uid.lo ^ uid.hi;
    }
    uint64_t gbuffer_key = forward_key ^ perturb;
    auto& pipeline_map = ctx.pipeline_map();
    PipelineCacheKey gkey{gbuffer_key, PixelFormat::Surface, target_group};
    if (auto it = pipeline_map.find(gkey); it != pipeline_map.end()) {
        return it->second;
    }

    string_view vsrc;
    string_view base_fsrc;
    string composed_fsrc;

    if (material_ptr) {
        auto* mat = interface_cast<IMaterial>(material_ptr.get());
        auto* src = interface_cast<IShaderSource>(material_ptr.get());
        if (mat && src) {
            auto eval_src = src->get_source(shader_role::kEval);
            auto vertex_src = src->get_source(shader_role::kVertex);
            auto eval_fn = src->get_fn_name(shader_role::kEval);
            if (!eval_src.empty() && !vertex_src.empty() && !eval_fn.empty()) {
                src->register_includes(ctx);
                composed_fsrc = compose_eval_fragment(
                    deferred_fragment_driver_template,
                    eval_src, eval_fn,
                    mat->get_deferred_discard_threshold());
                vsrc = vertex_src;
                base_fsrc = string_view(composed_fsrc);
            }
        }
    }
    if (base_fsrc.empty()) {
        base_fsrc = default_gbuffer_fragment_src;
    }
    if (vsrc.empty() && shader_source_ptr) {
        auto v = shader_source_ptr->get_source(shader_role::kVertex);
        if (!v.empty()) vsrc = v;
    }
    if (vsrc.empty()) {
        vsrc = default_gbuffer_vertex_src;
    }

    string composed;
    composed.append(base_fsrc);
    composed.append(string_view("\n", 1));
    string_view discard_def =
        shader_source_ptr
            ? shader_source_ptr->get_source(shader_role::kDiscard)
            : string_view{};
    if (!discard_def.empty()) {
        // Role::Discard returns the full `void velk_visual_discard()`
        // definition; the composer appends it verbatim after the
        // fragment driver template.
        composed.append(discard_def);
    } else {
        composed.append(string_view("void velk_visual_discard() {}\n", 30));
    }

    PipelineOptions po = batch.pipeline_options();
    // G-buffer passes always write opaquely regardless of alpha mode.
    po.blend_mode = BlendMode::Opaque;

    return ctx.compile_pipeline(
        string_view(composed), vsrc,
        gbuffer_key, PixelFormat::Surface, target_group, po);
}

} // namespace

uint64_t DeferredPath::ensure_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx || !ctx.snippets) return 0;

    const auto& intersect_ids = ctx.snippets->frame_intersects();
    const auto& intersect_info_by_id = ctx.snippets->intersect_info_by_id();
    const auto& shadow_tech_ids = ctx.snippets->frame_shadow_techs();
    const auto& shadow_tech_info = ctx.snippets->shadow_tech_info_by_id();

    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kDeferredTag = 0x44665232'44666572ULL;
    constexpr uint64_t kShadowTag = 0x53686477'54656368ULL;
    uint64_t key = kFnvBasis ^ kDeferredTag;
    for (auto id : intersect_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ kShadowTag) * kFnvPrime;
    for (auto id : shadow_tech_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key |= 0x4000000000000000ULL;

    if (compiled_pipelines_.find(key) != compiled_pipelines_.end()) {
        return key;
    }

    string src;
    src += default_deferred_compute_src;
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    append_literal("        case 255u: return intersect_mesh(ray, shape, hit);\n");
    char buf[128];
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");

    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }

    append_literal("float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal) {\n");
    append_literal("    switch (tech_id) {\n");
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ti.fn_name;
        append_literal("(light_idx, world_pos, world_normal);\n");
    }
    append_literal("        default: return 1.0;\n");
    append_literal("    }\n");
    append_literal("}\n");

    uint64_t compiled = ctx.render_ctx->compile_compute_pipeline(string_view(src), key);
    if (compiled == 0) return 0;
    compiled_pipelines_[key] = true;
    return key;
}

RenderTargetGroup DeferredPath::ensure_gbuffer(ViewState& vs, int width, int height,
                                               FrameContext& /*ctx*/,
                                               IRenderGraph& graph)
{
    if (width <= 0 || height <= 0) return 0;

    TextureGroupDesc gdesc{};
    gdesc.formats = array_view<const PixelFormat>(
        kGBufferFormats, static_cast<uint32_t>(GBufferAttachment::Count));
    gdesc.width = width;
    gdesc.height = height;
    gdesc.depth = DepthFormat::Default;
    // Per-frame allocation: drop the prior Ptr (manager parks the
    // group on its pool) and request a fresh one. Pool reuse kicks in
    // once the parked entry's GPU work has retired.
    vs.gbuffer = graph.resources().create_render_texture_group(gdesc);
    if (!vs.gbuffer) return 0;

    vs.gbuffer_width = width;
    vs.gbuffer_height = height;
    auto group = vs.gbuffer->get_gpu_handle(GpuResourceKey::Default);

    if (!vs.worldpos_alias) {
        vs.worldpos_alias = instance().create<IRenderTarget>(ClassId::RenderTexture);
    }
    if (vs.worldpos_alias) {
        auto wp_id = vs.gbuffer->attachment(
            static_cast<uint32_t>(GBufferAttachment::WorldPos));
        vs.worldpos_alias->set_gpu_handle(
            GpuResourceKey::Default, static_cast<uint64_t>(wp_id));
        vs.worldpos_alias->set_size(static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height));
    }
    return group;
}

void DeferredPath::build_passes(IViewEntry& entry,
                                const RenderView& render_view,
                                IRenderTarget::Ptr color_target,
                                FrameContext& ctx,
                                IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.material_cache || !ctx.pipeline_map) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) return;

    auto& vs = view_states_[&entry];

    auto gbuffer_handle = ensure_gbuffer(vs, render_view.width, render_view.height, ctx, graph);
    if (gbuffer_handle == 0) return;

    emit_gbuffer_pass(entry, vs, render_view, ctx, graph);

    if (vs.gbuffer_width <= 0 || vs.gbuffer_height <= 0) return;
    emit_lighting_pass(entry, vs, render_view, color_target, ctx,
                       vs.gbuffer_width, vs.gbuffer_height, graph);
}

void DeferredPath::emit_gbuffer_pass(IViewEntry& /*entry*/, ViewState& vs,
                                     const RenderView& render_view, FrameContext& ctx,
                                     IRenderGraph& graph)
{
    if (!render_view.batches) return;

    auto group_id = vs.gbuffer->get_gpu_handle(GpuResourceKey::Default);

    auto* default_uv1 = ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
    auto resolve = [&](const IBatch& b) {
        return resolve_or_compile_gbuffer(*ctx.render_ctx, b, group_id);
    };
    vector<DrawCall> gbuffer_draw_calls;
    emit_draw_calls(
        gbuffer_draw_calls,
        *render_view.batches,
        *ctx.frame_buffer,
        *ctx.resources,
        *ctx.material_cache,
        default_uv1,
        resolve,
        render_view.has_frustum ? &render_view.frustum : nullptr);

    auto gp = ::velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass);
    if (!gp) return;
    rect viewport{0, 0,
                  static_cast<float>(vs.gbuffer_width),
                  static_cast<float>(vs.gbuffer_height)};
    gp->add_op(ops::BeginPass{group_id});
    gp->add_op(ops::Submit{viewport, std::move(gbuffer_draw_calls)});
    gp->add_op(ops::EndPass{});
    gp->add_write(interface_pointer_cast<IGpuResource>(vs.gbuffer));
    gp->set_view_globals_address(render_view.view_globals_address);
    graph.add_pass(std::move(gp));
}

void DeferredPath::emit_lighting_pass(IViewEntry& /*entry*/, ViewState& vs,
                                      const RenderView& render_view,
                                      IRenderTarget::Ptr color_target,
                                      FrameContext& ctx,
                                      int w, int h,
                                      IRenderGraph& graph)
{
    // Per-frame allocation: drop the prior Ptr (manager parks the
    // handle on its pool) and request a fresh one each frame. Steady-
    // state hits the pool once the parked handle's GPU work retires.
    {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        // Lighting compute shader writes raw HDR linear radiance via
        // gStorageImagesF16 (binding 3). Tonemap, if requested, is a
        // separate post-process effect attached to the camera pipeline.
        td.format = PixelFormat::RGBA16F;
        td.usage = TextureUsage::Storage;
        vs.deferred_output = graph.resources().create_render_texture(td);
    }
    if (!vs.deferred_output) return;

    {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA32F;
        td.usage = TextureUsage::Storage;
        vs.shadow_debug = graph.resources().create_render_texture(td);
    }

    uint64_t pipeline_key = ensure_pipeline(ctx);
    if (pipeline_key == 0) return;
    auto pit = ctx.pipeline_map->find(
        PipelineCacheKey{pipeline_key, PixelFormat::Surface, 0});
    if (pit == ctx.pipeline_map->end()) return;

    auto albedo_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Albedo));
    auto normal_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
    auto worldpos_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
    auto material_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::MaterialParams));

    // Lights and env come pre-resolved from RenderView. ViewPreparer
    // registered shadow techniques against the snippet registry and
    // stamped flags[1] with the registered id; the deferred compute's
    // velk_eval_shadow switch is composed from the same registry, so
    // any tech ordering works.
    uint64_t lights_addr = 0;
    if (!render_view.lights.empty() && ctx.frame_buffer) {
        lights_addr = ctx.frame_buffer->write(
            render_view.lights.data(),
            render_view.lights.size() * sizeof(GpuLight));
    }

    VELK_GPU_STRUCT PushC {
        float    cam_pos[4];
        uint32_t output_image_id;
        uint32_t albedo_tex_id;
        uint32_t normal_tex_id;
        uint32_t worldpos_tex_id;
        uint32_t material_tex_id;
        uint32_t width;
        uint32_t height;
        uint32_t light_count;
        uint32_t env_texture_id;
        uint32_t shadow_debug_image_id;
        uint64_t lights_addr;
        uint64_t env_data_addr;
    };
    static_assert(sizeof(PushC) == 80, "Deferred PushC layout mismatch");

    PushC pc{};
    pc.cam_pos[0] = render_view.cam_pos.x;
    pc.cam_pos[1] = render_view.cam_pos.y;
    pc.cam_pos[2] = render_view.cam_pos.z;
    pc.cam_pos[3] = 0.f;
    pc.output_image_id = static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default));
    pc.albedo_tex_id   = albedo_id;
    pc.normal_tex_id   = normal_id;
    pc.worldpos_tex_id = worldpos_id;
    pc.material_tex_id = material_id;
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());
    pc.env_texture_id = render_view.env.texture_id;
    pc.shadow_debug_image_id = static_cast<uint32_t>(vs.shadow_debug->get_gpu_handle(GpuResourceKey::Default));
    pc.lights_addr = lights_addr;
    pc.env_data_addr = render_view.env.data_addr;

    auto gp = ::velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass);
    if (!gp) return;
    DispatchCall dc{};
    dc.pipeline = pit->second;
    dc.groups_x = (w + 7) / 8;
    dc.groups_y = (h + 7) / 8;
    dc.groups_z = 1;
    dc.root_constants_size = sizeof(PushC);
    std::memcpy(dc.root_constants, &pc, sizeof(PushC));
    gp->add_op(ops::Dispatch{dc});
    gp->add_op(ops::BlitToSurface{
        static_cast<TextureId>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default)),
        color_target ? color_target->get_gpu_handle(GpuResourceKey::Default) : 0,
        render_view.viewport});

    gp->add_read(interface_pointer_cast<IGpuResource>(vs.gbuffer));
    gp->add_write(interface_pointer_cast<IGpuResource>(vs.deferred_output));
    if (color_target) gp->add_write(interface_pointer_cast<IGpuResource>(color_target));
    gp->set_view_globals_address(render_view.view_globals_address);
    graph.add_pass(std::move(gp));
}

void DeferredPath::on_view_removed(IViewEntry& entry, FrameContext& /*ctx*/)
{
    // Erase the view state; gbuffer / deferred_output / shadow_debug
    // Ptrs drop, resource manager auto-defers the backend handles.
    view_states_.erase(&entry);
}

void DeferredPath::shutdown(FrameContext& /*ctx*/)
{
    view_states_.clear();
    compiled_pipelines_.clear();
}

IGpuResource::Ptr DeferredPath::find_named_output(string_view name, IViewEntry* view) const
{
    auto it = view_states_.find(view);
    if (it == view_states_.end()) return {};
    auto& vs = it->second;
    if (name == "gbuffer") {
        return interface_pointer_cast<IGpuResource>(vs.gbuffer);
    }
    if (name == "gbuffer.worldpos") {
        return interface_pointer_cast<IGpuResource>(vs.worldpos_alias);
    }
    if (name == "shadow.debug") {
        return interface_pointer_cast<IGpuResource>(vs.shadow_debug);
    }
    if (name == "output") {
        return interface_pointer_cast<IGpuResource>(vs.deferred_output);
    }
    return {};
}

} // namespace velk
