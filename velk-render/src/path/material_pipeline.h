#ifndef VELK_RENDER_PATH_MATERIAL_PIPELINE_H
#define VELK_RENDER_PATH_MATERIAL_PIPELINE_H

#include <velk/api/velk.h>
#include <velk/string_view.h>

#include <velk-render/frame/raster_shaders.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/material/intf_material.h>

namespace velk {

/**
 * @brief Compiles the forward variant of a material's pipeline.
 *
 * Composes the forward fragment shader from the material's IShaderSource
 * (eval source + eval entry point + `forward_fragment_driver_template`)
 * and calls `IRenderContext::compile_pipeline`. Returns the cache key
 * the call produced (also usable as the value the renderer caches the
 * compiled pipeline under at `(key, target_format, target_group=0)`).
 *
 * Returns 0 if the material lacks an eval/vertex source pair.
 */
inline uint64_t compile_material_forward_pipeline(
    IRenderContext& ctx, const IBatch& batch,
    PixelFormat target_format, uint64_t user_key)
{
    auto material_ptr = batch.material();
    if (!material_ptr) return 0;
    auto* mat = interface_cast<IMaterial>(material_ptr.get());
    auto* src = interface_cast<IShaderSource>(material_ptr.get());
    if (!mat || !src) return 0;
    auto eval_src = src->get_source(shader_role::kEval);
    auto vertex_src = src->get_source(shader_role::kVertex);
    auto eval_fn = src->get_fn_name(shader_role::kEval);
    if (eval_src.empty() || vertex_src.empty() || eval_fn.empty()) return 0;
    src->register_includes(ctx);
    string frag = compose_eval_fragment(
        forward_fragment_driver_template, eval_src, eval_fn,
        mat->get_forward_discard_threshold());
    return ctx.compile_pipeline(
        string_view(frag), vertex_src,
        user_key, target_format, /*target_group=*/0,
        batch.pipeline_options());
}

/**
 * @brief S6 dynamic-rendering variant of `compile_material_forward_pipeline`.
 *
 * Same shader composition; calls `compile_pipeline_dynamic` so the
 * resulting pipeline runs inside `record_begin_rendering` / `record_end_rendering`.
 * Cache slot is `(user_key, color_format, nullptr)` — single-color case
 * (ForwardPath canary). Returns 0 if the material lacks the source.
 */
inline uint64_t compile_material_forward_pipeline_dynamic(
    IRenderContext& ctx, const IBatch& batch,
    PixelFormat color_format, DepthFormat depth_format,
    uint64_t user_key)
{
    auto material_ptr = batch.material();
    if (!material_ptr) return 0;
    auto* mat = interface_cast<IMaterial>(material_ptr.get());
    auto* src = interface_cast<IShaderSource>(material_ptr.get());
    if (!mat || !src) return 0;
    auto eval_src = src->get_source(shader_role::kEval);
    auto vertex_src = src->get_source(shader_role::kVertex);
    auto eval_fn = src->get_fn_name(shader_role::kEval);
    if (eval_src.empty() || vertex_src.empty() || eval_fn.empty()) return 0;
    src->register_includes(ctx);
    string frag = compose_eval_fragment(
        forward_fragment_driver_template, eval_src, eval_fn,
        mat->get_forward_discard_threshold());
    PixelFormat formats[1] = {color_format};
    return ctx.compile_pipeline_dynamic(
        string_view(frag), vertex_src,
        user_key,
        array_view<const PixelFormat>(formats, 1),
        depth_format,
        batch.pipeline_options());
}

/**
 * @brief Returns the material's stable forward-pipeline cache key,
 *        compiling the forward variant if needed to bootstrap it.
 *
 * `IMaterial::get_pipeline_handle` doubles as the cache key paths use
 * to derive their own variants (e.g. DeferredPath's gbuffer key XORs
 * this with a per-visual perturb). The key is populated lazily when
 * ForwardPath first compiles the material. For deferred-only views
 * (no ForwardPath render of this material), the key would otherwise
 * stay 0 and the gbuffer / RT paths would silently skip the batch.
 *
 * This helper bridges that gap: if the key is 0, it compiles the
 * forward variant at `PixelFormat::Surface` and caches the resulting
 * key on the material. Subsequent calls return it directly.
 *
 * Returns 0 if the material lacks the source needed to compile.
 */
inline uint64_t ensure_material_forward_key(IRenderContext& ctx, const IBatch& batch)
{
    auto material_ptr = batch.material();
    if (!material_ptr) return 0;
    uint64_t key = material_ptr->get_pipeline_handle(ctx);
    if (key != 0) return key;
    uint64_t compiled = compile_material_forward_pipeline(
        ctx, batch, PixelFormat::Surface, /*user_key=*/0);
    if (compiled != 0) {
        material_ptr->set_pipeline_handle(compiled);
    }
    return compiled;
}

} // namespace velk

#endif // VELK_RENDER_PATH_MATERIAL_PIPELINE_H
