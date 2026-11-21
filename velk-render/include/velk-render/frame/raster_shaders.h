#ifndef VELK_RENDER_FRAME_RASTER_SHADERS_H
#define VELK_RENDER_FRAME_RASTER_SHADERS_H

#include <velk/string.h>
#include <velk/string_view.h>

#include <velk-render/ext/element_vertex.h>

#include <cstdio>
#include <cstring>

namespace velk {

// Default raster vertex/fragment pair used when a visual or material
// does not supply its own. Both forward and gbuffer default vertex
// shaders are the single shared element vertex shader.

[[maybe_unused]] constexpr string_view default_vertex_src =
    ::velk::ext::element_vertex_src;

[[maybe_unused]] constexpr string_view default_fragment_src = R"(
#version 450
#include "velk.glsl"

// Declares every varying the shared element vertex shader writes, even
// the ones this shader ignores, so the SPIR-V interface matches and the
// validator doesn't warn about dropped outputs.
VELK_VARYINGS_IN

VELK_FRAG_OUT(frag_color)

void main()
{
    frag_color = v_color;
}
)";

// The culling's trace of one pixel (InstanceCullPass::trace), for the visibility pass: at that
// pixel, what came of each fragment drawn there. Its notes are of stage 4: reached the shader (0),
// cut out by its material's alpha (1), written (2); then the draw, its ids' second word (instance
// and triangle) and its depth's bits. Only where fragment shaders can write buffers, which the
// composer says with VELK_FRAGMENT_STORES; else nothing.
#define VELK_TRACE_FRAGMENT_GLSL R"(
#ifdef VELK_FRAGMENT_STORES
layout(set = 1, binding = 16, std430) buffer VelkTraceWords { uint data[]; } velk_trace_words;

// A note of stage `stage` at the traced pixel, for a fragment of draw `draw_base`: what came of
// it, its ids' second word and `value`.
void velk_trace_fragment_note(uint stage, uint draw_base, uint outcome, uint value)
{
    uint t = velk_globals.data[velk_draw_data.data[draw_base].globals_base].trace;
    if (t == 0xFFFFFFFFu) return;
    if (uint(gl_FragCoord.x) != velk_trace_words.data[t] || uint(gl_FragCoord.y) != velk_trace_words.data[t + 1u]) return;
    uint at = atomicAdd(velk_trace_words.data[t + 2u], 1u);
    if (at >= velk_trace_words.data[t + 3u]) return;
    uint w = t + 6u + at * 4u;
    velk_trace_words.data[w] = stage | (outcome << 8u);
    velk_trace_words.data[w + 1u] = draw_base;
    velk_trace_words.data[w + 2u] = (v_instance << velk_draw_data.data[draw_base].triangle_bits) | uint(gl_PrimitiveID);
    velk_trace_words.data[w + 3u] = value;
}
#else
void velk_trace_fragment_note(uint stage, uint draw_base, uint outcome, uint value) {}
#endif

void velk_trace_fragment(uint draw_base, uint outcome)
{
    velk_trace_fragment_note(4u, draw_base, outcome, floatBitsToUint(gl_FragCoord.z));
}
)"

// Writes which triangle a pixel shows.
[[maybe_unused]] constexpr string_view visibility_fragment_src = R"(
#version 450
#include "velk.glsl"

VELK_DRAW_DATA(root)

VELK_VARYINGS_IN

VELK_VISIBILITY_OUT
)" VELK_TRACE_FRAGMENT_GLSL R"(
void main()
{
    velk_trace_fragment(root.draw_base, 2u);
    VELK_WRITE_VISIBILITY(root);
}
)";

// The same, for a visual that cuts out its own shape. The composer appends its discard snippet.
[[maybe_unused]] constexpr string_view visibility_discard_fragment_src = R"(
#version 450
#include "velk.glsl"

VELK_DRAW_DATA(root)

VELK_VARYINGS_IN

VELK_VISIBILITY_OUT
)" VELK_TRACE_FRAGMENT_GLSL R"(
void velk_visual_discard();

void main()
{
    velk_trace_fragment(root.draw_base, 0u);
    velk_visual_discard();
    velk_trace_fragment(root.draw_base, 2u);
    VELK_WRITE_VISIBILITY(root);
}
)";

// The same, for a material cut out by its alpha, composed with its eval. The composer appends
// the visual's discard snippet or an empty stub.
[[maybe_unused]] constexpr string_view visibility_masked_fragment_template = R"(
VELK_DRAW_DATA(root)

VELK_VARYINGS_IN

VELK_VISIBILITY_OUT
)" VELK_TRACE_FRAGMENT_GLSL R"(
void velk_visual_discard();

void main()
{
    GlobalData globals = velk_global_data(root);
    velk_trace_fragment(root.draw_base, 0u);
    velk_visual_discard();

    EvalContext ctx;
    ctx.material_base = velk_draw(root).material_base;
    ctx.texture_id  = velk_draw(root).texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_rel_pos);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_rel_pos + globals.cam_pos.xyz;
    ctx.rel_pos     = v_rel_pos;
    ctx.tangent     = v_world_tangent;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) {
        velk_trace_fragment(root.draw_base, 1u);
        discard;
    }

    velk_trace_fragment(root.draw_base, 2u);
    VELK_WRITE_VISIBILITY(root);
}
)";

// ===== Material eval-driver fragment templates =====
// Forward and deferred fragment shaders that materials with a
// `velk_eval_<name>` body share. The raster-pipeline composer
// concatenates these with the material's eval_src, vertex_src, and the
// visual's (optional) discard snippet, then compiles.
//
// Each template assumes:
//   - `velk.glsl` and `velk-ui.glsl` are #included by the composer
//     (which gives EvalContext / MaterialEval / VELK_LIGHTING_*).
//   - A literal  <%EVAL_FN%>  is replaced with the material's eval
//     function name (e.g. "velk_eval_gradient").
//   - A literal  <%DISCARD%>  is replaced with a float literal for the
//     alpha-discard threshold.

[[maybe_unused]] constexpr string_view forward_fragment_driver_template = R"(
VELK_DRAW_DATA(root)

VELK_VARYINGS_IN

VELK_FRAG_OUT(frag_color)
)" VELK_TRACE_FRAGMENT_GLSL R"(
void main()
{
    GlobalData globals = velk_global_data(root);
    EvalContext ctx;
    ctx.material_base = velk_draw(root).material_base;
    ctx.texture_id  = velk_draw(root).texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_rel_pos);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_rel_pos + globals.cam_pos.xyz;
    ctx.rel_pos     = v_rel_pos;
    ctx.tangent     = v_world_tangent;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;
    // The culling's trace, at its pixel: drawn by the forward path, at this depth.
    velk_trace_fragment_note(9u, root.draw_base, 0u, floatBitsToUint(gl_FragCoord.z));
    frag_color = e.color;
}
)";

// Transparent (glass) driver. Same eval-driven shading as the forward
// template, but the output opacity is boosted toward 1 at grazing angles via
// a Schlick-Fresnel term (dielectric F0 = 0.04). This gives a transmissive
// pane edge presence as the view angle steepens — the honest part of glass
// reflectance — without faking any reflected *color* (a sky/env reflection
// would be wrong when geometry sits between the glass and the sky). Used by
// the deferred path's transparent pass for BLEND / transmissive materials.
[[maybe_unused]] constexpr string_view transparent_fragment_driver_template = R"(
VELK_DRAW_DATA(root)

VELK_VARYINGS_IN

VELK_FRAG_OUT(frag_color)

void main()
{
    GlobalData globals = velk_global_data(root);
    EvalContext ctx;
    ctx.material_base = velk_draw(root).material_base;
    ctx.texture_id  = velk_draw(root).texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_rel_pos);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_rel_pos + globals.cam_pos.xyz;
    ctx.rel_pos     = v_rel_pos;
    ctx.tangent     = v_world_tangent;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;

    // An unlit material's alpha is its opacity as it stands.
    float opacity = e.color.a;
    if (e.lighting_mode == VELK_LIGHTING_STANDARD) {
        vec3 N = normalize(length(e.normal) > 0.0 ? e.normal : v_world_normal);
        vec3 V = normalize(-v_rel_pos);
        float ndv = clamp(dot(N, V), 0.0, 1.0);
        float fresnel = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
        opacity = e.color.a + fresnel * (1.0 - e.color.a);
    }
    frag_color = vec4(e.color.rgb, opacity);
}
)";

/**
 * @brief Composes a full fragment shader from a driver template and a
 *        material's eval source.
 *
 * Substitutes `<%EVAL_FN%>` with @p eval_fn and `<%DISCARD%>` with a
 * float literal of @p discard_threshold, then prepends a preamble
 * (`#version`, shared includes) and the material's @p eval_src.
 */
inline string compose_eval_fragment(string_view driver_template,
                                    string_view eval_src,
                                    string_view eval_fn,
                                    float discard_threshold)
{
    string out;
    out.append(string_view("#version 450\n"
                           "#include \"velk.glsl\"\n"
                           "#include \"velk-ui.glsl\"\n"));
    // A raster pipeline compiles for a single material, so VELK_MATERIAL(T)
    // binds the arena as a typed block and the snippet's load is a direct
    // indexed read. Those are the definitions declared in velk-ui.glsl, so
    // nothing to override here; the compute composer is the one that swaps
    // them out.
    out.append(eval_src);
    out.append(string_view("\n"));

    char thr_buf[32];
    int tn = std::snprintf(thr_buf, sizeof(thr_buf), "%f", discard_threshold);
    string_view thr(thr_buf, tn > 0 ? static_cast<size_t>(tn) : 0);

    size_t i = 0;
    while (i < driver_template.size()) {
        if (i + 10 <= driver_template.size()
            && std::memcmp(driver_template.data() + i, "<%EVAL_FN%>", 11) == 0) {
            out.append(eval_fn);
            i += 11;
        } else if (i + 11 <= driver_template.size()
                   && std::memcmp(driver_template.data() + i, "<%DISCARD%>", 11) == 0) {
            out.append(thr);
            i += 11;
        } else {
            out.append(string_view(driver_template.data() + i, 1));
            ++i;
        }
    }
    return out;
}

} // namespace velk

#endif // VELK_RENDER_FRAME_RASTER_SHADERS_H
