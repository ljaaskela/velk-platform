#ifndef VELK_RENDER_PATH_MATERIAL_PIPELINE_H
#define VELK_RENDER_PATH_MATERIAL_PIPELINE_H

#include <velk/api/velk.h>
#include <velk/string_view.h>

#include <velk-render/frame/raster_shaders.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/material/intf_material.h>

#include <cstring>

namespace velk {

/// Namespace tags for material pipeline content keys. XOR'd into the FNV
/// basis so a forward and a gbuffer variant of the same material never
/// collide even with identical inputs.
constexpr uint64_t kForwardEvalContentTag = 0x46776452'4576616cULL; // "FwdREval"
constexpr uint64_t kGbufferEvalContentTag = 0x47427546'4576616cULL; // "GBufEval"

/**
 * @brief Accumulates an FNV-1a content hash over the inputs that uniquely
 *        determine a compiled material pipeline (shader sources + the
 *        framework discard threshold + PipelineOptions).
 *
 * The driver template is constant per variant, so it isn't hashed — the
 * variant tag (passed to the constructor) covers it. Strings are length-
 * prefixed so concatenations can't alias. The result's top bit is set so a
 * content key can never collide with a small built-in `PipelineKey::*` or an
 * auto-assigned counter (neither sets that bit).
 */
struct PipelineContentHasher
{
    static constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    static constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    uint64_t h;

    explicit PipelineContentHasher(uint64_t tag) : h(kFnvBasis ^ tag) {}

    void byte(uint8_t b) { h = (h ^ b) * kFnvPrime; }

    void str(string_view s)
    {
        uint64_t n = s.size();
        for (int i = 0; i < 8; ++i) byte(static_cast<uint8_t>(n >> (i * 8)));
        for (char c : s) byte(static_cast<uint8_t>(c));
    }

    void f32(float f)
    {
        uint32_t u = 0;
        std::memcpy(&u, &f, sizeof(u));
        for (int i = 0; i < 4; ++i) byte(static_cast<uint8_t>(u >> (i * 8)));
    }

    void options(const PipelineOptions& o)
    {
        byte(static_cast<uint8_t>(o.topology));
        byte(static_cast<uint8_t>(o.cull_mode));
        byte(static_cast<uint8_t>(o.front_face));
        byte(static_cast<uint8_t>(o.blend_mode));
        byte(static_cast<uint8_t>(o.depth_test));
        byte(static_cast<uint8_t>(o.depth_write ? 1 : 0));
    }

    uint64_t key() const { return h | 0x2000000000000000ULL; }
};

/**
 * @brief Compiles the forward variant of a material's pipeline.
 *
 * Composes the forward fragment shader from the material's IShaderSource
 * (eval source + eval entry point + `forward_fragment_driver_template`)
 * and calls `IRenderContext::compile_pipeline_dynamic`. Returns the
 * compiled pipeline as a strong `Ptr` (the caller must keep it alive —
 * the cache holds only a weak ref); @p out_key receives the cache key.
 *
 * Returns nullptr if the material lacks an eval/vertex source pair.
 */
inline IGpuPipeline::Ptr compile_material_forward_pipeline_dynamic(
    IRenderContext& ctx, const IBatch& batch,
    PixelFormat color_format, DepthFormat depth_format,
    uint64_t user_key, uint64_t* out_key = nullptr)
{
    auto material_ptr = batch.material();
    if (!material_ptr) return {};
    auto* mat = interface_cast<IMaterial>(material_ptr);
    auto* src = interface_cast<IShaderSource>(material_ptr);
    if (!(mat && src)) {
        return {};
    }
    auto eval_src = src->get_source(shader_role::kEval);
    auto vertex_src = src->get_source(shader_role::kVertex);
    auto eval_fn = src->get_fn_name(shader_role::kEval);
    if (eval_src.empty() || vertex_src.empty() || eval_fn.empty()) return {};
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
        batch.pipeline_options(),
        out_key);
}

/**
 * @brief Content key for a material's FORWARD eval pipeline.
 *
 * FNV-1a over the material's eval + vertex source, eval entry point, forward
 * discard threshold, and the batch's PipelineOptions — everything (besides
 * the constant driver template, covered by the variant tag) that determines
 * the compiled forward pipeline. Identical materials therefore share one
 * pipeline, and a churned-then-recreated material hits the cache instead of
 * recompiling. Registers the material's shader includes as a side effect (the
 * deferred gbuffer compile relies on them being present).
 *
 * Returns 0 if the material has no usable eval/vertex source pair (raw-
 * fragment material or non-material program), so callers keep their existing
 * key for those.
 */
inline uint64_t forward_material_content_key(IRenderContext& ctx, const IBatch& batch)
{
    auto material_ptr = batch.material();
    if (!material_ptr) return 0;
    auto* mat = interface_cast<IMaterial>(material_ptr);
    auto* src = interface_cast<IShaderSource>(material_ptr);
    if (!(mat && src)) return 0;
    auto eval_src = src->get_source(shader_role::kEval);
    auto vertex_src = src->get_source(shader_role::kVertex);
    auto eval_fn = src->get_fn_name(shader_role::kEval);
    if (eval_src.empty() || vertex_src.empty() || eval_fn.empty()) return 0;
    src->register_includes(ctx);
    PipelineContentHasher hasher(kForwardEvalContentTag);
    hasher.str(eval_src);
    hasher.str(vertex_src);
    hasher.str(eval_fn);
    hasher.f32(mat->get_forward_discard_threshold());
    hasher.options(batch.pipeline_options());
    return hasher.key();
}

/**
 * @brief Returns the material's stable forward-pipeline cache key, minting it
 *        from content on first request.
 *
 * `IMaterial::get_pipeline_handle` caches the key; paths use it as the
 * material's identity (e.g. the deferred path's skip guard, and the lazy
 * include-registration the gbuffer compile depends on). The key is the
 * forward content key (see `forward_material_content_key`), so it is stable
 * across recreation of an identical material rather than a per-instance id.
 *
 * Returns 0 if the material lacks the eval source needed to render, so the
 * deferred / RT paths skip the batch as before.
 */
inline uint64_t ensure_material_forward_key(IRenderContext& ctx, const IBatch& batch)
{
    auto material_ptr = batch.material();
    if (!material_ptr) return 0;
    uint64_t key = material_ptr->get_pipeline_handle(ctx);
    if (key != 0) return key;
    key = forward_material_content_key(ctx, batch);
    if (key != 0) {
        material_ptr->set_pipeline_handle(key);
    }
    return key;
}

/**
 * @brief Resolves (or lazy-compiles) the forward-rendering pipeline for a
 *        batch against the given color/depth attachment formats.
 *
 * Material wins over the visual's IShaderSource when both are present; the
 * visual's source is the no-material fallback. Eval materials key on their
 * content so identical materials share a pipeline; raw-fragment materials keep
 * their stored handle; non-material programs keep their explicit pipeline_key.
 * The batch's own `pipeline_options()` (blend / depth / cull) drive the
 * pipeline state. Returns nullptr to skip (no source / compile failure).
 *
 * Shared by ForwardPath and the deferred path's transparent (blended) pass.
 */
inline IGpuPipeline::Ptr resolve_or_compile_forward(IRenderContext& ctx,
                                                    const IBatch& batch,
                                                    PixelFormat target_format,
                                                    DepthFormat depth_format)
{
    auto material_ptr = batch.material();
    auto shader_source_ptr = batch.shader_source();
    const bool use_material = (material_ptr != nullptr);
    PipelineOptions pipeline_options = batch.pipeline_options();
    uint64_t user_key = 0;
    if (use_material) {
        user_key = forward_material_content_key(ctx, batch);
        if (user_key == 0) {
            user_key = material_ptr->get_pipeline_handle(ctx);
        }
    } else {
        user_key = batch.pipeline_key();
    }

    if (auto pipeline = ctx.find_pipeline(
            PipelineCacheKey{user_key, target_format, depth_format, 0})) {
        return pipeline;
    }

    // Cache miss: compile and return the strong Ptr directly (the cache holds
    // only a weak ref, so a find-after-compile would already be dead).
    IGpuPipeline::Ptr compiled;
    uint64_t compiled_key = 0;
    if (use_material) {
        compiled = compile_material_forward_pipeline_dynamic(
            ctx, batch, target_format, depth_format, user_key, &compiled_key);
        if (!compiled) {
            auto* src = interface_cast<IShaderSource>(material_ptr);
            auto vertex_src = src ? src->get_source(shader_role::kVertex) : string_view{};
            auto frag_src = src ? src->get_source(shader_role::kFragment) : string_view{};
            if (!frag_src.empty() && !vertex_src.empty()) {
                PixelFormat formats[1] = {target_format};
                compiled = ctx.compile_pipeline_dynamic(
                    frag_src, vertex_src,
                    user_key,
                    array_view<const PixelFormat>(formats, 1),
                    depth_format,
                    pipeline_options, &compiled_key);
            }
        }
        if (compiled_key && material_ptr->get_pipeline_handle(ctx) == 0) {
            material_ptr->set_pipeline_handle(compiled_key);
        }
    } else if (shader_source_ptr && user_key != 0) {
        auto vsrc = shader_source_ptr->get_source(shader_role::kVertex);
        auto fsrc = shader_source_ptr->get_source(shader_role::kFragment);
        PixelFormat formats[1] = {target_format};
        compiled = ctx.compile_pipeline_dynamic(
            fsrc, vsrc,
            user_key,
            array_view<const PixelFormat>(formats, 1),
            depth_format,
            pipeline_options);
    }

    return compiled;
}

} // namespace velk

#endif // VELK_RENDER_PATH_MATERIAL_PIPELINE_H
