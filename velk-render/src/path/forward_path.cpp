#include "path/forward_path.h"

#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/frame/raster_shaders.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/plugin.h>

namespace velk {

namespace {

/// Resolves (or lazy-compiles) the forward-rendering pipeline for a
/// batch. Material wins over the visual's IShaderSource when both
/// are present; the visual's source is the no-material fallback.
/// Returns 0 to skip (no source / compile failure).
PipelineId resolve_or_compile_forward(IRenderContext& ctx,
                                      const IBatch& batch,
                                      PixelFormat target_format)
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
        return it->second;
    }

    uint64_t compiled_key = 0;
    if (use_material) {
        if (auto* mat = interface_cast<IMaterial>(material_ptr)) {
            auto* src = interface_cast<IShaderSource>(material_ptr);
            auto eval_src = src ? src->get_source(shader_role::kEval) : string_view{};
            auto vertex_src = src ? src->get_source(shader_role::kVertex) : string_view{};
            auto eval_fn = src ? src->get_fn_name(shader_role::kEval) : string_view{};
            auto frag_src = src ? src->get_source(shader_role::kFragment) : string_view{};
            if (!eval_src.empty() && !vertex_src.empty() && !eval_fn.empty()) {
                src->register_includes(ctx);
                string frag = compose_eval_fragment(
                    forward_fragment_driver_template, eval_src, eval_fn,
                    mat->get_forward_discard_threshold());
                compiled_key = ctx.compile_pipeline(
                    string_view(frag), vertex_src,
                    user_key, target_format, 0,
                    pipeline_options);
            } else if (!frag_src.empty() && !vertex_src.empty()) {
                compiled_key = ctx.compile_pipeline(
                    frag_src, vertex_src,
                    user_key, target_format, 0,
                    pipeline_options);
            }
            if (compiled_key && material_ptr->get_pipeline_handle(ctx) == 0) {
                material_ptr->set_pipeline_handle(compiled_key);
            }
        }
    } else if (shader_source_ptr && user_key != 0) {
        auto vsrc = shader_source_ptr->get_source(shader_role::kVertex);
        auto fsrc = shader_source_ptr->get_source(shader_role::kFragment);
        compiled_key = ctx.compile_pipeline(
            fsrc, vsrc,
            user_key, target_format, 0,
            pipeline_options);
    }

    if (compiled_key == 0) return 0;

    if (auto it = pipeline_map.find(
            PipelineCacheKey{compiled_key, target_format, 0});
        it != pipeline_map.end()) {
        return it->second;
    }
    return 0;
}

} // namespace

void ForwardPath::build_passes(IViewEntry& entry,
                               const RenderView& render_view,
                               IRenderTarget::Ptr color_target,
                               FrameContext& ctx,
                               IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.material_cache
        || !ctx.pipeline_map || !ctx.render_ctx) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) return;

    const ::velk::render::Frustum* frustum_ptr =
        render_view.has_frustum ? &render_view.frustum : nullptr;
    auto& mcache = *ctx.material_cache;

    auto* default_uv1 = ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
    auto target_format = ctx.target_format;

    auto resolve = [&](const IBatch& b) {
        return resolve_or_compile_forward(*ctx.render_ctx, b, target_format);
    };

    vector<DrawCall> draw_calls;

    // Env first (no frustum cull — fullscreen). Null env_batch means
    // no env on this view's camera.
    if (render_view.env_batch && render_view.env_batch->material()) {
        vector<IBatch::Ptr> env_batches{render_view.env_batch};
        emit_draw_calls(
            draw_calls,
            env_batches, *ctx.frame_buffer, *ctx.resources,
            mcache,
            default_uv1, resolve, /*frustum=*/nullptr);
    }

    // Main scene batches.
    if (render_view.batches && !render_view.batches->empty()) {
        emit_draw_calls(
            draw_calls,
            *render_view.batches, *ctx.frame_buffer, *ctx.resources,
            mcache,
            default_uv1, resolve, frustum_ptr);
    }

    auto pass = ::velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass);
    if (!pass) return;
    uint64_t target_id = color_target
        ? color_target->get_gpu_handle(GpuResourceKey::Default)
        : 0;
    pass->add_op(ops::BeginPass{target_id});
    pass->add_op(ops::Submit{render_view.viewport, std::move(draw_calls)});
    pass->add_op(ops::EndPass{});
    if (color_target) {
        pass->add_write(interface_pointer_cast<IGpuResource>(color_target));
    }
    pass->set_view_globals_address(render_view.view_globals_address);
    graph.add_pass(std::move(pass));
}

} // namespace velk
