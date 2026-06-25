#include "path/deferred_path.h"
#include "path/material_pipeline.h"

#include <velk-render/api/cached_view_pass.h>

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
IGpuPipeline::Ptr resolve_or_compile_gbuffer(IRenderContext& ctx,
                                             const IBatch& batch)
{
    auto material_ptr = batch.material();
    auto shader_source_ptr = batch.shader_source();

    // Forward cache key bootstrap: the gbuffer key is derived from the
    // forward key, which is populated lazily by ForwardPath compiling
    // the material. For deferred-only views (where ForwardPath never
    // runs for this material) the helper compiles the forward variant
    // here so the key is stable. Without it, the batch would be
    // silently skipped from the gbuffer pass.
    uint64_t forward_key = batch.pipeline_key();
    if (forward_key == 0) {
        forward_key = ensure_material_forward_key(ctx, batch);
    }
    if (forward_key == 0) return {};

    // Pull the material's eval snippet for the gbuffer compile below.
    // Fast-path note: when ensure_material_forward_key just compiled,
    // the same source is fetched twice — cheap (string_view copies),
    // and keeps the helper composable.
    auto* mat = material_ptr ? interface_cast<IMaterial>(material_ptr.get()) : nullptr;
    auto* src = material_ptr ? interface_cast<IShaderSource>(material_ptr.get()) : nullptr;
    string_view eval_src;
    string_view vertex_src;
    string_view eval_fn;
    bool has_eval = false;
    if (mat && src) {
        eval_src = src->get_source(shader_role::kEval);
        vertex_src = src->get_source(shader_role::kVertex);
        eval_fn = src->get_fn_name(shader_role::kEval);
        if (!eval_src.empty() && !vertex_src.empty() && !eval_fn.empty()) {
            has_eval = true;
        }
    }

    uint64_t gbuffer_key;
    if (has_eval) {
        // Content key: identical materials share one gbuffer pipeline and a
        // recreated material hits the cache. Captures the deferred eval inputs
        // (the deferred driver template is constant, covered by the tag).
        string_view discard_def =
            shader_source_ptr ? shader_source_ptr->get_source(shader_role::kDiscard)
                              : string_view{};
        PipelineOptions po = batch.pipeline_options();
        po.blend_mode = BlendMode::Opaque; // gbuffer always writes opaquely
        PipelineContentHasher hasher(kGbufferEvalContentTag);
        hasher.str(eval_src);
        hasher.str(eval_fn);
        hasher.f32(mat->get_deferred_discard_threshold());
        hasher.str(discard_def);
        hasher.str(vertex_src);
        hasher.options(po);
        gbuffer_key = hasher.key();
    } else {
        // Non-eval batch (default gbuffer shader): keep the per-instance key
        // XOR'd with a per-visual-class perturb so two visuals sharing a
        // material still get distinct velk_visual_discard bodies.
        uint64_t perturb = 0;
        if (auto* obj = interface_cast<IObject>(shader_source_ptr.get())) {
            Uid uid = obj->get_class_uid();
            perturb = uid.lo ^ uid.hi;
        }
        gbuffer_key = forward_key ^ perturb;
    }
    // Cache lookup must match what compile_pipeline_dynamic stores:
    // color_formats[0] (Albedo = RGBA8) plus the MRT layout signature
    // derived from the full gbuffer format set (stable across resize).
    const auto gbuffer_formats = array_view<const PixelFormat>(
        kGBufferFormats, static_cast<uint32_t>(GBufferAttachment::Count));
    PipelineCacheKey gkey{gbuffer_key, kGBufferFormats[0], DepthFormat::Default,
                          pipeline_target_layout(gbuffer_formats)};
    if (auto pipeline = ctx.find_pipeline(gkey)) {
        return pipeline;
    }

    string_view vsrc;
    string_view base_fsrc;
    string composed_fsrc;
    if (has_eval) {
        composed_fsrc = compose_eval_fragment(
            deferred_fragment_driver_template,
            eval_src, eval_fn,
            mat->get_deferred_discard_threshold());
        vsrc = vertex_src;
        base_fsrc = string_view(composed_fsrc);
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

    // Gbuffer compiles against kGBufferFormats + DepthFormat::Default. The
    // layout signature (computed inside compile_pipeline_dynamic from these
    // formats) differentiates gbuffer pipelines from forward ones using the
    // same user_key.
    // Compile and return the strong Ptr directly (cache is weak, so a
    // find-after-compile would already be dead).
    return ctx.compile_pipeline_dynamic(
        string_view(composed), vsrc,
        gbuffer_key, gbuffer_formats,
        DepthFormat::Default,
        po);
}

} // namespace

namespace {

// CPU mirror of the deferred lighting push-constant block
// (`layout(push_constant) PC` in deferred_compute_prelude_src).
VELK_GPU_STRUCT DeferredComputePushC {
    uint64_t globals;          // FrameGlobals BDA, GLSL pc.globals
    float    cam_pos[4];
    uint32_t output_image_id;
    uint32_t albedo_tex_id;
    uint32_t normal_tex_id;
    uint32_t worldpos_tex_id;
    uint32_t material_tex_id;
    uint32_t emissive_tex_id;
    uint32_t width;
    uint32_t height;
    uint32_t light_count;
    uint32_t env_texture_id;
    uint32_t shadow_debug_image_id;
    uint32_t _pad0;            // aligns the BDA pointers below to 8
    uint64_t lights_addr;
    uint64_t env_data_addr;
    uint32_t irr_image_id;     // demodulated diffuse irradiance output
    uint32_t _pad1;            // pads to 96 (VELK_GPU_STRUCT is alignas(16))
};
static_assert(sizeof(DeferredComputePushC) == 96, "Deferred compute PushC layout mismatch");

// Push constants for the diffuse-irradiance TEMPORAL pass (scalar layout;
// mirrors the PC block in deferred_denoise_compute_src).
VELK_GPU_STRUCT DenoisePushC {
    uint64_t globals;
    uint32_t normal_id;
    uint32_t worldpos_id;
    uint32_t irr_id;
    uint32_t hist_irr_prev_id;
    uint32_t hist_pos_prev_id;
    uint32_t hist_irr_cur_id;
    uint32_t hist_pos_cur_id;
    uint32_t width;
    uint32_t height;
    uint32_t reset;
};

// Push constants for the spatial filter + composite pass (mirrors the PC block
// in deferred_spatial_composite_compute_src).
VELK_GPU_STRUCT SpatialPushC {
    uint64_t globals;
    uint32_t albedo_id;
    uint32_t material_id;
    uint32_t normal_id;
    uint32_t worldpos_id;
    uint32_t hist_irr_id;
    uint32_t output_id;
    uint32_t width;
    uint32_t height;
};

// Composes a deferred-family compute pipeline: the shared prelude
// (deferred_compute_prelude_src) + the given main() + the intersect_shape
// and velk_eval_shadow switches built from the frame's snippet set. The
// lighting and sun-visibility shaders are identical except for main(), so
// they compose the same way and differ only by `main_src` + `variant_tag`
// (the tag keeps their weak-cache entries distinct).
IGpuPipeline::Ptr compose_deferred_compute_pipeline(FrameContext& ctx,
                                                    string_view main_src,
                                                    uint64_t variant_tag)
{
    if (!ctx.render_ctx || !ctx.snippets) return {};

    const auto& intersect_ids = ctx.snippets->frame_intersects();
    const auto& intersect_info_by_id = ctx.snippets->intersect_info_by_id();
    const auto& shadow_tech_ids = ctx.snippets->frame_shadow_techs();
    const auto& shadow_tech_info = ctx.snippets->shadow_tech_info_by_id();

    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kDeferredTag = 0x44665232'44666572ULL;
    constexpr uint64_t kShadowTag = 0x53686477'54656368ULL;
    uint64_t key = kFnvBasis ^ kDeferredTag;
    key = (key ^ variant_tag) * kFnvPrime;
    for (auto id : intersect_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ kShadowTag) * kFnvPrime;
    for (auto id : shadow_tech_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key |= 0x4000000000000000ULL;

    // The weak pipeline cache is the source of truth: if the pipeline for
    // this snippet combo is still alive (held by a live pass), reuse it;
    // otherwise compose + compile a fresh one.
    if (auto p = ctx.render_ctx->find_pipeline(
            PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0})) {
        return p;
    }

    string src;
    src += deferred_compute_prelude_src;
    src += main_src;
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

    return ctx.render_ctx->compile_compute_pipeline(string_view(src), key);
}

} // namespace

IGpuPipeline::Ptr DeferredPath::ensure_pipeline(FrameContext& ctx)
{
    return compose_deferred_compute_pipeline(ctx, deferred_lighting_main_src, 0ull);
}

IGpuPipeline::Ptr DeferredPath::ensure_denoise_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx) return {};
    // Standalone compute; fixed key (no snippet variance). Bit 62 set to match
    // the compute-pipeline key convention.
    constexpr uint64_t key = 0x4465'6e6f'6973'6531ULL;
    if (auto p = ctx.render_ctx->find_pipeline(
            PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0})) {
        return p;
    }
    return ctx.render_ctx->compile_compute_pipeline(deferred_denoise_compute_src, key);
}

IGpuPipeline::Ptr DeferredPath::ensure_spatial_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx) return {};
    constexpr uint64_t key = 0x5370'6174'6961'6c31ULL;
    if (auto p = ctx.render_ctx->find_pipeline(
            PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0})) {
        return p;
    }
    return ctx.render_ctx->compile_compute_pipeline(deferred_spatial_composite_compute_src, key);
}

IRenderTextureGroup* DeferredPath::ensure_gbuffer(ViewState& vs, int width, int height,
                                                  FrameContext& /*ctx*/,
                                                  IRenderGraph& graph)
{
    if (width <= 0 || height <= 0) return nullptr;

    uvec2 want{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (vs.gbuffer && vs.gbuffer_size == want) {
        return vs.gbuffer.get();
    }

    TextureGroupDesc gdesc{};
    gdesc.formats = array_view<const PixelFormat>(
        kGBufferFormats, static_cast<uint32_t>(GBufferAttachment::Count));
    gdesc.width = width;
    gdesc.height = height;
    gdesc.depth = DepthFormat::Default;
    vs.gbuffer = graph.resources().create_render_texture_group(gdesc);
    if (!vs.gbuffer) return nullptr;

    vs.gbuffer_size = want;
    vs.gbuffer_dirty = true;
    vs.lighting_dirty = true;
    vs.transparent_dirty = true;

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
    return vs.gbuffer.get();
}

void DeferredPath::build_passes(IViewEntry& entry,
                                const RenderView& render_view,
                                IRenderTarget::Ptr color_target,
                                FrameContext& ctx,
                                IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.render_ctx) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) return;

    auto [it, inserted] = view_states_.try_emplace(&entry);
    auto& vs = it->second;
    if (inserted) {
        entry.add_render_state_observer(this);
    }

    auto* gbuffer_handle = ensure_gbuffer(vs, render_view.width, render_view.height, ctx, graph);
    if (!gbuffer_handle) return;

    emit_gbuffer_pass(entry, vs, render_view, ctx, graph);

    if (vs.gbuffer_size.x == 0 || vs.gbuffer_size.y == 0) return;
    emit_lighting_pass(entry, vs, render_view, color_target, ctx,
                       static_cast<int>(vs.gbuffer_size.x),
                       static_cast<int>(vs.gbuffer_size.y), graph);
    // Temporally accumulate the noisy diffuse irradiance (writes history), then
    // spatially filter it (count-driven) + composite the final image + blit.
    emit_temporal_pass(entry, vs, render_view, ctx,
                       static_cast<int>(vs.gbuffer_size.x),
                       static_cast<int>(vs.gbuffer_size.y), graph);
    emit_spatial_composite_pass(entry, vs, render_view, color_target, ctx,
                                static_cast<int>(vs.gbuffer_size.x),
                                static_cast<int>(vs.gbuffer_size.y), graph);
    // Blended geometry draws forward, over the lit composite, against the
    // retained gbuffer depth.
    emit_transparent_pass(entry, vs, render_view, color_target, ctx, graph);
}

void DeferredPath::emit_gbuffer_pass(IViewEntry& /*entry*/, ViewState& vs,
                                     const RenderView& render_view, FrameContext& ctx,
                                     IRenderGraph& graph)
{
    if (!render_view.batches) return;

    emit_cached_view_pass(
        vs.cached_gbuffer_pass, vs.gbuffer_dirty, render_view.view_globals_address, graph,
        [&](CachedPassRecording& rec) {
            IRenderTextureGroup* group = vs.gbuffer.get();
            auto* default_uv1 = ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
            // Hold strong refs to every pipeline this pass binds (cache is
            // weak). resolve runs before set_held_pipelines replaces the
            // pass's old set, so reused pipelines never drop to zero refs.
            auto resolve = [&](const IBatch& b) -> IGpuPipeline* {
                // Blended materials draw in the transparent pass, not the
                // opaque G-buffer; skip them here.
                if (b.pipeline_options().blend_mode == BlendMode::Alpha) {
                    return nullptr;
                }
                auto p = resolve_or_compile_gbuffer(*ctx.render_ctx, b);
                if (!p) return nullptr;
                IGpuPipeline* raw = p.get();
                rec.held.push_back(std::move(p));
                return raw;
            };
            vector<DrawCall> gbuffer_draw_calls;
            emit_draw_calls(
                gbuffer_draw_calls,
                *render_view.batches,
                *ctx.frame_buffer,
                *ctx.resources,
                default_uv1,
                render_view.view_globals_address,
                resolve,
                render_view.has_frustum ? &render_view.frustum : nullptr);

            rect viewport{0, 0,
                          static_cast<float>(vs.gbuffer_size.x),
                          static_cast<float>(vs.gbuffer_size.y)};

            // Gbuffer raster runs as a self-contained dynamic-rendering
            // secondary. Attachments bound inline via `record_begin_rendering`;
            // executor doesn't wrap in begin_pass / end_pass.
            if (auto cmd = ctx.backend->create_command_buffer()) {
                constexpr uint32_t kColorCount = static_cast<uint32_t>(GBufferAttachment::Count);
                ColorAttachment colors[kColorCount]{};
                for (uint32_t i = 0; i < kColorCount; ++i) {
                    colors[i].texture = group->attachment_texture(i);
                    colors[i].clear = true;
                    colors[i].clear_color[0] = 0.f;
                    colors[i].clear_color[1] = 0.f;
                    colors[i].clear_color[2] = 0.f;
                    colors[i].clear_color[3] = 0.f;
                }
                DepthAttachment depth_att{};
                depth_att.texture = group->depth_attachment();
                depth_att.clear = true;
                depth_att.clear_depth = 1.0f;
                depth_att.clear_stencil = 0;

                cmd->begin_recording();
                cmd->push_label("DeferredPath: gbuffer");
                cmd->record_begin_rendering(
                    array_view<const ColorAttachment>(colors, kColorCount),
                    depth_att.texture ? &depth_att : nullptr);
                cmd->set_viewport(viewport);
                cmd->record_draws({gbuffer_draw_calls.data(), gbuffer_draw_calls.size()});
                cmd->record_end_rendering();
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            // Self-contained cmd buffer; no target_* seam.
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        });
}

void DeferredPath::emit_lighting_pass(IViewEntry& /*entry*/, ViewState& vs,
                                      const RenderView& render_view,
                                      IRenderTarget::Ptr color_target,
                                      FrameContext& ctx,
                                      int w, int h,
                                      IRenderGraph& graph)
{
    // Persistent allocation: keep the same Ptrs across frames so
    // downstream PushC bindless ids and `add_write` resource refs stay
    // stable. Recreate only on size change (or first-time). Recreation
    // invalidates the cached lighting pass (PushC ids change).
    uvec2 want{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    if (!vs.deferred_output || vs.output_size != want) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        // Lighting compute shader writes raw HDR linear radiance via
        // gStorageImagesF16 (binding 3). Tonemap, if requested, is a
        // separate post-process effect attached to the camera pipeline.
        td.format = PixelFormat::RGBA16F;
        td.usage = TextureUsage::Storage;
        vs.deferred_output = graph.resources().create_render_texture(td);
        vs.deferred_output_tex = graph.resources().find_texture(vs.deferred_output.get());
        vs.lighting_dirty = true;
    }
    if (!vs.deferred_output) return;

    if (!vs.shadow_debug || vs.output_size != want) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA32F;
        td.usage = TextureUsage::Storage;
        vs.shadow_debug = graph.resources().create_render_texture(td);
        vs.shadow_debug_tex = graph.resources().find_texture(vs.shadow_debug.get());
        vs.lighting_dirty = true;
    }

    // Demodulated diffuse irradiance: the lighting pass writes the noisy
    // single-light estimate here; the denoise pass accumulates + composites it.
    if (!vs.diffuse_irr || vs.output_size != want) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA16F;
        td.usage = TextureUsage::Storage;
        vs.diffuse_irr = graph.resources().create_render_texture(td);
        vs.diffuse_irr_tex = graph.resources().find_texture(vs.diffuse_irr.get());
        vs.lighting_dirty = true;
    }
    vs.output_size = want;

    auto lighting_pipeline = ensure_pipeline(ctx);
    if (!lighting_pipeline) return;

    auto albedo_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Albedo));
    auto normal_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
    auto worldpos_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
    auto material_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::MaterialParams));
    auto emissive_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Emissive));

    // Lights and env come pre-resolved from RenderView. ViewPreparer
    // registered shadow techniques against the snippet registry and
    // stamped flags[1] with the registered id; the deferred compute's
    // velk_eval_shadow switch is composed from the same registry, so
    // any tech ordering works.
    // Persistent per-view lights buffer staged by ViewPreparer; address
    // is stable across frames so cached lighting passes can embed it.
    uint64_t lights_addr = render_view.lights_addr;

    DeferredComputePushC pc{};
    pc.globals = render_view.view_globals_address;
    pc.cam_pos[0] = render_view.cam_pos.x;
    pc.cam_pos[1] = render_view.cam_pos.y;
    pc.cam_pos[2] = render_view.cam_pos.z;
    pc.cam_pos[3] = 0.f;
    pc.output_image_id = static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default));
    pc.albedo_tex_id   = albedo_id;
    pc.normal_tex_id   = normal_id;
    pc.worldpos_tex_id = worldpos_id;
    pc.material_tex_id = material_id;
    pc.emissive_tex_id = emissive_id;
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());
    pc.env_texture_id = render_view.env.texture_id;
    pc.shadow_debug_image_id = static_cast<uint32_t>(vs.shadow_debug->get_gpu_handle(GpuResourceKey::Default));
    pc.lights_addr = lights_addr;
    pc.env_data_addr = render_view.env.data_addr;
    pc.irr_image_id = static_cast<uint32_t>(vs.diffuse_irr->get_gpu_handle(GpuResourceKey::Default));

    // No surface blit here anymore: the lighting pass writes the "rest" image
    // (deferred_output) + diffuse irradiance; the denoise/composite pass
    // produces the final image and blits it.
    emit_cached_view_pass(
        vs.cached_lighting_pass, vs.lighting_dirty, render_view.view_globals_address, graph,
        [&](CachedPassRecording& rec) {
            DispatchCall dc{};
            dc.pipeline = lighting_pipeline.get();
            dc.groups_x = (w + 7) / 8;
            dc.groups_y = (h + 7) / 8;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));

            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("DeferredPath: lighting");
                cmd->record_dispatch(dc);
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.diffuse_irr));
            // Hold the lighting compute pipeline strong (cache is weak).
            rec.held.push_back(std::move(lighting_pipeline));
        });
}

void DeferredPath::emit_temporal_pass(IViewEntry& /*entry*/, ViewState& vs,
                                      const RenderView& render_view,
                                      FrameContext& ctx,
                                      int w, int h,
                                      IRenderGraph& graph)
{
    if (!vs.diffuse_irr || !vs.gbuffer) return;

    uvec2 want{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    if (!vs.hist_irr[0] || vs.hist_size != want) {
        auto make = [&]() {
            TextureDesc td{};
            td.width = w;
            td.height = h;
            td.format = PixelFormat::RGBA16F;
            td.usage = TextureUsage::Storage;
            return graph.resources().create_render_texture(td);
        };
        for (int i = 0; i < 2; ++i) {
            vs.hist_irr[i] = make();
            vs.hist_pos[i] = make();
            vs.hist_irr_tex[i] = graph.resources().find_texture(vs.hist_irr[i].get());
            vs.hist_pos_tex[i] = graph.resources().find_texture(vs.hist_pos[i].get());
        }
        vs.hist_size = want;
        vs.denoise_reset_pending = true; // fresh (garbage) history
    }
    if (!vs.hist_irr[0] || !vs.hist_irr[1]) return;

    auto temporal_pipeline = ensure_denoise_pipeline(ctx);
    if (!temporal_pipeline) return;

    const uint32_t parity = static_cast<uint32_t>(ctx.present_counter & 1ull);

    DenoisePushC pc{};
    pc.globals = render_view.view_globals_address;
    pc.normal_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
    pc.worldpos_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
    pc.irr_id      = static_cast<uint32_t>(vs.diffuse_irr->get_gpu_handle(GpuResourceKey::Default));
    pc.hist_irr_prev_id = static_cast<uint32_t>(vs.hist_irr[parity]->get_gpu_handle(GpuResourceKey::Default));
    pc.hist_pos_prev_id = static_cast<uint32_t>(vs.hist_pos[parity]->get_gpu_handle(GpuResourceKey::Default));
    pc.hist_irr_cur_id  = static_cast<uint32_t>(vs.hist_irr[1u - parity]->get_gpu_handle(GpuResourceKey::Default));
    pc.hist_pos_cur_id  = static_cast<uint32_t>(vs.hist_pos[1u - parity]->get_gpu_handle(GpuResourceKey::Default));
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    pc.reset  = vs.denoise_reset_pending ? 1u : 0u;
    vs.denoise_reset_pending = false;

    // Ping-pong history ids change every frame -> re-record each frame (one
    // dispatch, negligible CPU).
    vs.denoise_dirty = true;

    IRenderTarget::Ptr hist_prev_irr = vs.hist_irr[parity];
    IRenderTarget::Ptr hist_prev_pos = vs.hist_pos[parity];
    IRenderTarget::Ptr hist_cur_irr  = vs.hist_irr[1u - parity];
    IRenderTarget::Ptr hist_cur_pos  = vs.hist_pos[1u - parity];

    emit_cached_view_pass(
        vs.cached_denoise_pass, vs.denoise_dirty, render_view.view_globals_address, graph,
        [&](CachedPassRecording& rec) {
            DispatchCall dc{};
            dc.pipeline = temporal_pipeline.get();
            dc.groups_x = (w + 7) / 8;
            dc.groups_y = (h + 7) / 8;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));

            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("DeferredPath: diffuse temporal accumulate");
                cmd->record_dispatch(dc);
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.diffuse_irr));
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(hist_prev_irr));
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(hist_prev_pos));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(hist_cur_irr));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(hist_cur_pos));
            rec.held.push_back(std::move(temporal_pipeline));
        });
}

void DeferredPath::emit_spatial_composite_pass(IViewEntry& /*entry*/, ViewState& vs,
                                               const RenderView& render_view,
                                               IRenderTarget::Ptr color_target,
                                               FrameContext& ctx,
                                               int w, int h,
                                               IRenderGraph& graph)
{
    if (!vs.deferred_output || !vs.gbuffer) return;

    const uint32_t parity = static_cast<uint32_t>(ctx.present_counter & 1ull);
    // The temporal pass wrote hist_irr[1 - parity] this frame.
    IRenderTarget::Ptr cur_hist = vs.hist_irr[1u - parity];
    if (!cur_hist) return;

    auto spatial_pipeline = ensure_spatial_pipeline(ctx);
    if (!spatial_pipeline) return;

    // Resolve the surface composite as an IGpuTexture for the final blit.
    IGpuTexture* src_tex = graph.resources().find_texture(vs.deferred_output.get());
    if (!src_tex) src_tex = vs.deferred_output_tex;
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) {
            dst_tex = graph.resources().find_texture(color_target.get());
            if (!dst_tex && ctx.resources) {
                dst_tex = ctx.resources->find_texture(color_target.get());
            }
        }
    }
    if (dst_tex != vs.last_spatial_dst) {
        vs.spatial_dirty = true;
        vs.last_spatial_dst = dst_tex;
    }

    SpatialPushC pc{};
    pc.globals     = render_view.view_globals_address;
    pc.albedo_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Albedo));
    pc.material_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::MaterialParams));
    pc.normal_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
    pc.worldpos_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
    pc.hist_irr_id = static_cast<uint32_t>(cur_hist->get_gpu_handle(GpuResourceKey::Default));
    pc.output_id   = static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default));
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);

    // The accumulated-history id alternates each frame (ping-pong) -> re-record
    // each frame (one dispatch + blit, negligible CPU).
    vs.spatial_dirty = true;

    emit_cached_view_pass(
        vs.cached_spatial_pass, vs.spatial_dirty, render_view.view_globals_address, graph,
        [&](CachedPassRecording& rec) {
            DispatchCall dc{};
            dc.pipeline = spatial_pipeline.get();
            dc.groups_x = (w + 7) / 8;
            dc.groups_y = (h + 7) / 8;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));

            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("DeferredPath: diffuse spatial + composite");
                cmd->record_dispatch(dc);
                if (src_tex && dst_tex) {
                    cmd->record_blit_to_texture(*src_tex, *dst_tex, render_view.viewport);
                }
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(cur_hist));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
            if (color_target) {
                rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
            }
            rec.held.push_back(std::move(spatial_pipeline));
        });
}

void DeferredPath::emit_transparent_pass(IViewEntry& /*entry*/, ViewState& vs,
                                         const RenderView& render_view,
                                         IRenderTarget::Ptr color_target,
                                         FrameContext& ctx,
                                         IRenderGraph& graph)
{
    if (!render_view.batches || render_view.batches->empty()) return;
    if (!color_target || !vs.gbuffer || !ctx.backend || !ctx.frame_buffer) return;

    // Resolve the composite target as an IGpuTexture (surface composite casts
    // directly; a RenderTexture proxy needs find_texture).
    IGpuTexture* target_texture = interface_cast<IGpuTexture>(color_target.get());
    if (!target_texture) {
        target_texture = graph.resources().find_texture(color_target.get());
        if (!target_texture && ctx.resources) {
            target_texture = ctx.resources->find_texture(color_target.get());
        }
    }
    if (!target_texture) return;

    IGpuTexture* depth_texture = vs.gbuffer->depth_attachment();

    // Retarget / resize detection: the cached cmd buffer bakes the composite
    // VkImage handle.
    if (target_texture != vs.last_transparent_target) {
        vs.transparent_dirty = true;
        vs.last_transparent_target = target_texture;
    }

    emit_cached_view_pass(
        vs.cached_transparent_pass, vs.transparent_dirty,
        render_view.view_globals_address, graph,
        [&](CachedPassRecording& rec) {
            auto* default_uv1 =
                ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
            const PixelFormat target_format = ctx.target_format;
            const DepthFormat depth_format = color_target->get_depth_format();

            // Only BLEND batches; everything else drew opaque in the gbuffer.
            // resolve returns null to skip non-blended batches, and holds the
            // pipeline strong (the cache is weak).
            auto resolve = [&](const IBatch& b) -> IGpuPipeline* {
                if (b.pipeline_options().blend_mode != BlendMode::Alpha) {
                    return nullptr;
                }
                // Glass driver: Fresnel-boosted opacity at grazing angles.
                auto p = resolve_or_compile_forward(
                    *ctx.render_ctx, b, target_format, depth_format,
                    transparent_fragment_driver_template, kTransparentEvalContentTag);
                if (!p) return nullptr;
                IGpuPipeline* raw = p.get();
                rec.held.push_back(std::move(p));
                return raw;
            };

            vector<DrawCall> draw_calls;
            emit_draw_calls(
                draw_calls, *render_view.batches, *ctx.frame_buffer, *ctx.resources,
                default_uv1, render_view.view_globals_address, resolve,
                render_view.has_frustum ? &render_view.frustum : nullptr);

            // No transparent geometry this view: leave an empty (no-op) pass.
            if (draw_calls.empty()) return;

            if (auto cmd = ctx.backend->create_command_buffer()) {
                // Load the lit composite + the opaque gbuffer depth; blend the
                // transparent draws over them (pipelines carry BlendMode::Alpha
                // and depth_write=false from the materials' options).
                ColorAttachment color{};
                color.texture = target_texture;
                color.clear = false;

                DepthAttachment depth_att{};
                depth_att.texture = depth_texture;
                depth_att.clear = false;

                cmd->begin_recording();
                cmd->push_label("DeferredPath: transparent");
                cmd->record_begin_rendering(
                    array_view<const ColorAttachment>(&color, 1),
                    depth_texture ? &depth_att : nullptr);
                cmd->set_viewport(render_view.viewport);
                cmd->record_draws({draw_calls.data(), draw_calls.size()});
                cmd->record_end_rendering();
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }

            // Reads the gbuffer (its depth); reads + writes the composite.
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        });
}

DeferredPath::~DeferredPath()
{
    // Detach from every view we observed. The renderer's destruction
    // order keeps IViewEntry::Ptrs alive past path destruction, so
    // these calls are safe.
    for (auto& [view, _] : view_states_) {
        view->remove_render_state_observer(this);
    }
}

void DeferredPath::on_render_state_changed(IRenderState* source,
                                           RenderStateChange /*flags*/)
{
    auto* view = interface_cast<IViewEntry>(source);
    if (!view) return;
    auto it = view_states_.find(view);
    if (it != view_states_.end()) {
        it->second.gbuffer_dirty = true;
        it->second.lighting_dirty = true;
        it->second.transparent_dirty = true;
    }
}

void DeferredPath::on_view_removed(IViewEntry& entry, FrameContext& /*ctx*/)
{
    auto it = view_states_.find(&entry);
    if (it == view_states_.end()) return;
    entry.remove_render_state_observer(this);
    // Erase the view state; gbuffer / deferred_output / shadow_debug
    // Ptrs drop, resource manager auto-defers the backend handles.
    view_states_.erase(it);
}

void DeferredPath::shutdown(FrameContext& /*ctx*/)
{
    for (auto& [view, _] : view_states_) {
        view->remove_render_state_observer(this);
    }
    view_states_.clear();
}

IGpuResource::Ptr DeferredPath::find_named_output(string_view name, IViewEntry* view) const
{
    auto it = view_states_.find(view);
    if (it == view_states_.end()) return {};
    auto& vs = it->second;
    if (name == "gbuffer") {
        return interface_pointer_cast<IGpuResource>(vs.gbuffer);
    }
    auto wrap = [](IGpuTexture* tex) -> IGpuResource::Ptr {
        return tex ? IGpuResource::Ptr(static_cast<IGpuResource*>(tex))
                   : IGpuResource::Ptr{};
    };
    if (name == "gbuffer.worldpos") {
        // Return the underlying IGpuTexture (cast as IGpuResource) so
        // debug code can `interface_cast<IGpuTexture>` and dump it.
        return wrap(vs.gbuffer
                ? vs.gbuffer->attachment_texture(
                      static_cast<uint32_t>(GBufferAttachment::WorldPos))
                : nullptr);
    }
    if (name == "shadow.debug") {
        return wrap(vs.shadow_debug_tex);
    }
    if (name == "output") {
        return wrap(vs.deferred_output_tex);
    }
    return {};
}

} // namespace velk
