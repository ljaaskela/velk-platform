#include "reference_path.h"

#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/frame/compute_shaders.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk {

namespace {

VELK_GPU_STRUCT ReferencePushC
{
    uint32_t restart;
    uint32_t globals_base;
    uint32_t light_count;
    uint32_t lights_base;
    float env_params[2];
    uint32_t extras[4];
    uint32_t env[4];
};

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

uint64_t hash_bytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * kFnvPrime;
    }
    return hash;
}

} // namespace

IGpuPipeline::Ptr ReferencePath::ensure_pipeline(FrameContext& ctx)
{
    if (!ctx.render_ctx || !ctx.snippets) {
        return {};
    }
    constexpr uint64_t kTag = 0x5265666572656e63ULL;
    uint64_t key = kFnvBasis ^ kTag;
    for (auto id : ctx.snippets->frame_materials()) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ 0xdeadbeefULL) * kFnvPrime;
    for (auto id : ctx.snippets->frame_shadow_techs()) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ 0xfeedfaceULL) * kFnvPrime;
    for (auto id : ctx.snippets->frame_intersects()) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key |= 0x8000000000000000ULL;

    if (auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0})) {
        return found;
    }
    const string source = compose_rt_compute(*ctx.snippets, rt_reference_main_src);
    return ctx.render_ctx->pipelines().compile_compute(string_view(source), key);
}

void ReferencePath::build_passes(IViewEntry& entry, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                 FrameContext& ctx, IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.render_ctx || !ctx.resources || render_view.width <= 0 || render_view.height <= 0) {
        return;
    }
    auto& vs = view_states_[&entry];
    const int w = render_view.width;
    const int h = render_view.height;
    const uvec2 want{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};

    bool restart = false;
    if (!vs.sum || vs.size != want) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA32F;
        td.usage = TextureUsage::Storage;
        td.sampler.mag_filter = SamplerFilter::Nearest;
        td.sampler.min_filter = SamplerFilter::Nearest;
        td.sampler.mipmap_mode = SamplerMipmapMode::Nearest;
        vs.sum = graph.resources().create_render_texture(td);
        td.format = PixelFormat::RGBA16F;
        vs.shown = graph.resources().create_render_texture(td);
        vs.size = want;
        restart = true;
    }
    if (!vs.sum || !vs.shown) {
        return;
    }

    uint64_t setup = kFnvBasis;
    setup = hash_bytes(setup, &render_view.relative_view_projection, sizeof(render_view.relative_view_projection));
    setup = hash_bytes(setup, &render_view.cam_pos, sizeof(render_view.cam_pos));
    setup = hash_bytes(setup, &render_view.env.texture_id, sizeof(render_view.env.texture_id));
    setup = hash_bytes(setup, &render_view.env.intensity, sizeof(render_view.env.intensity));
    setup = hash_bytes(setup, &render_view.env.rotation_rad, sizeof(render_view.env.rotation_rad));
    // A light's position is left out, so that a swaying lamp does not start it over.
    for (const GpuLight& light : render_view.lights) {
        setup = hash_bytes(setup, light.color_intensity, sizeof(light.color_intensity));
        if (light.flags[0] == 0u) {
            setup = hash_bytes(setup, light.direction, sizeof(light.direction));
        }
    }
    restart = restart || setup != vs.setup;
    vs.setup = setup;

    auto pipeline = ensure_pipeline(ctx);
    if (!pipeline) {
        return;
    }

    IGpuTexture* shown_tex = graph.resources().find_texture(vs.shown.get());
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) dst_tex = graph.resources().find_texture(color_target.get());
        if (!dst_tex) dst_tex = ctx.resources->find_texture(color_target.get());
    }

    const auto handle = [](const IRenderTarget::Ptr& t) {
        return static_cast<uint32_t>(t->get_gpu_handle(GpuResourceKey::Default));
    };

    ReferencePushC pc{};
    pc.restart = restart ? 1u : 0u;
    pc.globals_base = render_view.view_globals_base;
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());
    pc.lights_base = render_view.lights_base;
    pc.extras[0] = handle(vs.sum);
    pc.extras[1] = handle(vs.shown);
    pc.extras[2] = static_cast<uint32_t>(w);
    pc.extras[3] = static_cast<uint32_t>(h);
    pc.env[1] = render_view.env.texture_id;
    std::memcpy(&pc.env[2], &render_view.env.intensity, sizeof(float));
    std::memcpy(&pc.env[3], &render_view.env.rotation_rad, sizeof(float));

    bool dirty = true;
    emit_cached_view_pass(vs.pass, dirty, "reference", graph, [&](CachedPassRecording& rec) {
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (w + 7) / 8;
        dc.groups_y = (h + 7) / 8;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));

        if (auto cmd = ctx.backend->create_command_buffer()) {
            cmd->begin_recording();
            cmd->record_dispatch(dc);
            if (shown_tex && dst_tex) {
                cmd->record_blit_to_texture(*shown_tex, *dst_tex, render_view.viewport);
            }
            cmd->end_recording();
            rec.cmd = std::move(cmd);
        }
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.sum));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.sum));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.shown));
        if (color_target) {
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        }
        rec.held.push_back(std::move(pipeline));
    });
}

void ReferencePath::on_view_removed(IViewEntry& entry, FrameContext& /*ctx*/)
{
    view_states_.erase(&entry);
}

void ReferencePath::shutdown(FrameContext& /*ctx*/)
{
    view_states_.clear();
}

} // namespace velk
