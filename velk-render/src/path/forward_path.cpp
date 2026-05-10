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
/// `dynamic_rendering` selects the S6 dynamic-rendering compile path
/// (cmd buffer self-contained via `record_begin_rendering`); the
/// legacy render-pass path stays for surface-target raster until S6.4.
/// Returns 0 to skip (no source / compile failure).
IGpuPipeline* resolve_or_compile_forward(IRenderContext& ctx,
                                         const IBatch& batch,
                                         PixelFormat target_format,
                                         bool dynamic_rendering)
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
        // Try the eval-snippet path first (material composed with the
        // forward driver template). Falls back to a raw fragment
        // source if the material declines the snippet path.
        if (dynamic_rendering) {
            compiled_key = compile_material_forward_pipeline_dynamic(
                ctx, batch, target_format, DepthFormat::None, user_key);
        } else {
            compiled_key = compile_material_forward_pipeline(
                ctx, batch, target_format, user_key);
        }
        if (compiled_key == 0) {
            auto* src = interface_cast<IShaderSource>(material_ptr);
            auto vertex_src = src ? src->get_source(shader_role::kVertex) : string_view{};
            auto frag_src = src ? src->get_source(shader_role::kFragment) : string_view{};
            if (!frag_src.empty() && !vertex_src.empty()) {
                if (dynamic_rendering) {
                    PixelFormat formats[1] = {target_format};
                    compiled_key = ctx.compile_pipeline_dynamic(
                        frag_src, vertex_src,
                        user_key,
                        array_view<const PixelFormat>(formats, 1),
                        DepthFormat::None,
                        pipeline_options);
                } else {
                    compiled_key = ctx.compile_pipeline(
                        frag_src, vertex_src,
                        user_key, target_format, 0,
                        pipeline_options);
                }
            }
        }
        if (compiled_key && material_ptr->get_pipeline_handle(ctx) == 0) {
            material_ptr->set_pipeline_handle(compiled_key);
        }
    } else if (shader_source_ptr && user_key != 0) {
        auto vsrc = shader_source_ptr->get_source(shader_role::kVertex);
        auto fsrc = shader_source_ptr->get_source(shader_role::kFragment);
        if (dynamic_rendering) {
            PixelFormat formats[1] = {target_format};
            compiled_key = ctx.compile_pipeline_dynamic(
                fsrc, vsrc,
                user_key,
                array_view<const PixelFormat>(formats, 1),
                DepthFormat::None,
                pipeline_options);
        } else {
            compiled_key = ctx.compile_pipeline(
                fsrc, vsrc,
                user_key, target_format, 0,
                pipeline_options);
        }
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

    if (!cache.dirty) {
        // Steady state: same Ptr, refresh only the per-frame view
        // globals address (FrameGlobals lives in per-frame staging
        // and rotates each frame).
        cache.pass->set_view_globals_address(render_view.view_globals_address);
        RENDER_LOG("forward.cached view=%p pass=%p target=%llu vg=0x%llx",
                   (void*)&entry, (void*)cache.pass.get(),
                   (unsigned long long)cache.pass->target_id(),
                   (unsigned long long)render_view.view_globals_address);
        graph.add_pass(cache.pass);
        return;
    }

    const ::velk::render::Frustum* frustum_ptr =
        render_view.has_frustum ? &render_view.frustum : nullptr;

    auto* default_uv1 = ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
    auto target_format = ctx.target_format;

    // S6.2 canary: dynamic rendering for texture targets. Surface
    // targets stay on the legacy render-pass path until S6.4 introduces
    // acquire_swapchain_texture; ensures CameraPipeline + the no-post-
    // process direct-to-swapchain path keep working during the slice.
    IGpuTexture* target_texture = nullptr;
    if (color_target) {
        target_texture = graph.resources().find_texture(color_target.get());
        if (!target_texture && ctx.resources) {
            target_texture = ctx.resources->find_texture(color_target.get());
        }
    }
    const bool use_dynamic_rendering = (target_texture != nullptr);

    auto resolve = [&](const IBatch& b) {
        return resolve_or_compile_forward(*ctx.render_ctx, b, target_format,
                                          use_dynamic_rendering);
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

    uint64_t target_id = 0;
    if (!use_dynamic_rendering && color_target) {
        // Legacy surface-target path: cmd buffer inherits the surface's
        // render pass; executor wraps execute() in begin_pass(target_id) /
        // end_pass(). Switches off in S6.4 once acquire_swapchain_texture
        // lets the surface flow through the dynamic-rendering branch too.
        target_id = color_target->get_gpu_handle(GpuResourceKey::Default);
    }

    RENDER_LOG("forward.rebuild view=%p dynamic=%d target_id=%llu draws=%zu",
               (void*)&entry, use_dynamic_rendering ? 1 : 0,
               (unsigned long long)target_id, draw_calls.size());

    if (use_dynamic_rendering) {
        // Self-contained secondary: record_begin_rendering binds the
        // attachment inline at record time; the executor doesn't wrap
        // execute() in begin_pass / end_pass.
        IGpuCommandBuffer::Ptr cmd = ctx.backend->create_command_buffer(/*target_id=*/0);
        if (cmd) {
            ColorAttachment color{};
            color.texture = target_texture;
            color.clear = true;
            // Forward fullscreen draws cover the target; clear-to-zero
            // is fine and matches the legacy render pass's loadOp=CLEAR.
            color.clear_color[0] = 0.f;
            color.clear_color[1] = 0.f;
            color.clear_color[2] = 0.f;
            color.clear_color[3] = 0.f;

            cmd->begin_recording();
            cmd->record_begin_rendering(
                array_view<const ColorAttachment>(&color, 1),
                /*depth=*/nullptr);
            cmd->set_viewport(render_view.viewport);
            cmd->record_draws({draw_calls.data(), draw_calls.size()});
            cmd->record_end_rendering();
            cmd->end_recording();
            cache.pass->set_command_buffer(std::move(cmd));
        }
        // No target_* seam: cmd buffer self-contained.
    } else {
        IGpuCommandBuffer::Ptr cmd = ctx.backend->create_command_buffer(target_id);
        if (cmd) {
            cmd->begin_recording();
            cmd->set_viewport(render_view.viewport);
            cmd->record_draws({draw_calls.data(), draw_calls.size()});
            cmd->end_recording();
            cache.pass->set_command_buffer(std::move(cmd));
        }
        cache.pass->set_target_id(target_id);
    }

    if (color_target) {
        cache.pass->add_write(interface_pointer_cast<IGpuResource>(color_target));
    }
    cache.pass->set_view_globals_address(render_view.view_globals_address);
    cache.dirty = false;
    graph.add_pass(cache.pass);
}

} // namespace velk
