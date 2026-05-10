#include "path/forward_path.h"
#include "path/material_pipeline.h"

#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/frame/raster_shaders.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/plugin.h>

namespace velk {

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

namespace {

/// Resolves (or lazy-compiles) the forward-rendering pipeline for a
/// batch. Material wins over the visual's IShaderSource when both
/// are present; the visual's source is the no-material fallback.
/// All pipelines compile against dynamic-rendering attachment formats
/// (S6 — see design-notes/render_dynamic_rendering.md). Returns 0 to
/// skip (no source / compile failure).
IGpuPipeline* resolve_or_compile_forward(IRenderContext& ctx,
                                         const IBatch& batch,
                                         PixelFormat target_format,
                                         DepthFormat depth_format)
{
    auto material_ptr = batch.material();
    auto shader_source_ptr = batch.shader_source();
    const bool use_material = (material_ptr != nullptr);
    PipelineOptions pipeline_options = batch.pipeline_options();
    uint64_t user_key = use_material
        ? material_ptr->get_pipeline_handle(ctx)
        : batch.pipeline_key();

    auto& pipeline_map = ctx.pipeline_map();
    if (auto it = pipeline_map.find(
            PipelineCacheKey{user_key, target_format, 0});
        it != pipeline_map.end()) {
        return it->second.get();
    }

    uint64_t compiled_key = 0;
    if (use_material) {
        compiled_key = compile_material_forward_pipeline_dynamic(
            ctx, batch, target_format, depth_format, user_key);
        if (compiled_key == 0) {
            auto* src = interface_cast<IShaderSource>(material_ptr);
            auto vertex_src = src ? src->get_source(shader_role::kVertex) : string_view{};
            auto frag_src = src ? src->get_source(shader_role::kFragment) : string_view{};
            if (!frag_src.empty() && !vertex_src.empty()) {
                PixelFormat formats[1] = {target_format};
                compiled_key = ctx.compile_pipeline_dynamic(
                    frag_src, vertex_src,
                    user_key,
                    array_view<const PixelFormat>(formats, 1),
                    depth_format,
                    pipeline_options);
            }
        }
        if (compiled_key && material_ptr->get_pipeline_handle(ctx) == 0) {
            material_ptr->set_pipeline_handle(compiled_key);
        }
    } else if (shader_source_ptr && user_key != 0) {
        auto vsrc = shader_source_ptr->get_source(shader_role::kVertex);
        auto fsrc = shader_source_ptr->get_source(shader_role::kFragment);
        PixelFormat formats[1] = {target_format};
        compiled_key = ctx.compile_pipeline_dynamic(
            fsrc, vsrc,
            user_key,
            array_view<const PixelFormat>(formats, 1),
            depth_format,
            pipeline_options);
    }

    if (compiled_key == 0) return nullptr;

    if (auto it = pipeline_map.find(
            PipelineCacheKey{compiled_key, target_format, 0});
        it != pipeline_map.end()) {
        return it->second.get();
    }
    return nullptr;
}

} // namespace

ForwardPath::~ForwardPath()
{
    // Detach from every view we observed. The renderer's destruction
    // order keeps IViewEntry::Ptrs alive past path destruction, so
    // these calls are safe.
    for (auto& [view, _] : cached_passes_) {
        view->remove_render_state_observer(this);
    }
}

void ForwardPath::on_render_state_changed(IRenderState* source,
                                          RenderStateChange /*flags*/)
{
    auto* view = interface_cast<IViewEntry>(source);
    if (!view) return;
    auto it = cached_passes_.find(view);
    if (it != cached_passes_.end()) {
        it->second.dirty = true;
    }
}

void ForwardPath::on_view_removed(IViewEntry& view, FrameContext& /*ctx*/)
{
    auto it = cached_passes_.find(&view);
    if (it == cached_passes_.end()) return;
    view.remove_render_state_observer(this);
    cached_passes_.erase(it);
}

void ForwardPath::build_passes(IViewEntry& entry,
                               const RenderView& render_view,
                               IRenderTarget::Ptr color_target,
                               FrameContext& ctx,
                               IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.frame_buffer
        || !ctx.pipeline_map || !ctx.render_ctx) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) return;

    // Get-or-create + first-sight subscription. The pass is rebuilt
    // only when `dirty` is set by `on_render_state_changed` (view's
    // batch set changed); steady-state frames refresh only the
    // per-frame `view_globals_address` on the cached pass.
    auto [it, inserted] = cached_passes_.try_emplace(&entry);
    auto& cache = it->second;
    if (inserted) {
        entry.add_render_state_observer(this);
    }
    if (!cache.pass) {
        cache.pass = ::velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass);
        if (!cache.pass) return;
        cache.dirty = true;
    }

    // S6.4: color_target may be the per-surface composite (a real
    // IGpuTexture-castable wrapper) or a RenderTexture proxy from
    // RenderTargetCache (not IGpuTexture; needs find_texture lookup).
    IGpuTexture* target_texture = nullptr;
    if (color_target) {
        target_texture = interface_cast<IGpuTexture>(color_target.get());
        if (!target_texture) {
            target_texture = graph.resources().find_texture(color_target.get());
            if (!target_texture && ctx.resources) {
                target_texture = ctx.resources->find_texture(color_target.get());
            }
        }
    }
    if (!target_texture) return;

    // Resize detection: if the target's IGpuTexture* changed (e.g.,
    // surface composite recreated on resize, RTT resized), the cached
    // cmd buffer's baked VkImage handles are stale and must re-record.
    if (target_texture != cache.last_target_texture) {
        cache.dirty = true;
        cache.last_target_texture = target_texture;
    }

    if (!cache.dirty) {
        // Steady state: same Ptr, refresh only the per-frame view
        // globals address (FrameGlobals lives in per-frame staging
        // and rotates each frame).
        cache.pass->set_view_globals_address(render_view.view_globals_address);
        RENDER_LOG("forward.cached view=%p pass=%p vg=0x%llx",
                   (void*)&entry, (void*)cache.pass.get(),
                   (unsigned long long)render_view.view_globals_address);
        graph.add_pass(cache.pass);
        return;
    }

    const ::velk::render::Frustum* frustum_ptr =
        render_view.has_frustum ? &render_view.frustum : nullptr;

    auto* default_uv1 = ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
    auto target_format = ctx.target_format;

    // Pair-depth allocation: only when the color_target is tagged with
    // a depth format (CameraPipeline does this for its path_color).
    // RTT cache leaves depth_format=None and gets no depth attachment.
    const DepthFormat target_depth_format = color_target
        ? color_target->get_depth_format()
        : DepthFormat::None;
    const uvec2 target_size = target_texture->get_dimensions();
    if (target_depth_format != DepthFormat::None) {
        if (!cache.depth_texture || cache.depth_size != target_size) {
            cache.depth_texture = ctx.backend->create_depth_attachment_texture(
                static_cast<int>(target_size.x),
                static_cast<int>(target_size.y),
                target_depth_format);
            cache.depth_size = target_size;
            cache.dirty = true;
        }
    } else if (cache.depth_texture) {
        cache.depth_texture.reset();
        cache.depth_size = uvec2{};
        cache.dirty = true;
    }
    IGpuTexture* depth_texture = cache.depth_texture.get();

    auto resolve = [&](const IBatch& b) {
        return resolve_or_compile_forward(*ctx.render_ctx, b, target_format,
                                          target_depth_format);
    };

    vector<DrawCall> draw_calls;

    // Env first (no frustum cull — fullscreen). Null env_batch means
    // no env on this view's camera.
    if (render_view.env_batch && render_view.env_batch->material()) {
        vector<IBatch::Ptr> env_batches{render_view.env_batch};
        emit_draw_calls(
            draw_calls,
            env_batches, *ctx.frame_buffer, *ctx.resources,
            default_uv1, render_view.view_globals_address,
            resolve, /*frustum=*/nullptr);
    }

    // Main scene batches.
    if (render_view.batches && !render_view.batches->empty()) {
        emit_draw_calls(
            draw_calls,
            *render_view.batches, *ctx.frame_buffer, *ctx.resources,
            default_uv1, render_view.view_globals_address,
            resolve, frustum_ptr);
    }

    cache.pass->reset();

    RENDER_LOG("forward.rebuild view=%p draws=%zu depth=%d",
               (void*)&entry, draw_calls.size(), depth_texture ? 1 : 0);

    // Self-contained dynamic-rendering secondary: attachments bound
    // inline at record time, executor doesn't wrap in begin_pass.
    IGpuCommandBuffer::Ptr cmd = ctx.backend->create_command_buffer();
    if (cmd) {
        ColorAttachment color{};
        color.texture = target_texture;
        color.clear = true;
        color.clear_color[0] = 0.f;
        color.clear_color[1] = 0.f;
        color.clear_color[2] = 0.f;
        color.clear_color[3] = 0.f;

        DepthAttachment depth_att{};
        if (depth_texture) {
            depth_att.texture = depth_texture;
            depth_att.clear = true;
            depth_att.clear_depth = 1.0f;
            depth_att.clear_stencil = 0;
        }

        cmd->begin_recording();
        cmd->record_begin_rendering(
            array_view<const ColorAttachment>(&color, 1),
            depth_texture ? &depth_att : nullptr);
        cmd->set_viewport(render_view.viewport);
        cmd->record_draws({draw_calls.data(), draw_calls.size()});
        cmd->record_end_rendering();
        cmd->end_recording();
        cache.pass->set_command_buffer(std::move(cmd));
    }

    if (color_target) {
        cache.pass->add_write(interface_pointer_cast<IGpuResource>(color_target));
    }
    cache.pass->set_view_globals_address(render_view.view_globals_address);
    cache.dirty = false;
    graph.add_pass(cache.pass);
}

} // namespace velk
