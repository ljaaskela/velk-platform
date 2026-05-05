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

// The shared element vertex shader writes the full canonical varying
// set (locations 0..6). Declare every input even when unused so the
// SPIR-V interface matches and the validator doesn't warn about
// dropped outputs.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

[[maybe_unused]] constexpr string_view default_gbuffer_vertex_src =
    ::velk::ext::element_vertex_src;

// Default fragment shader for the deferred G-buffer fill. Writes the
// instance color straight through to albedo, marks the fragment as
// "unlit" so the compute lighting pass passes albedo through unchanged.
// Materials that want lighting (StandardMaterial) override this.
[[maybe_unused]] constexpr string_view default_gbuffer_fragment_src = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

// G-buffer attachment locations. Must match deferred_gbuffer.h's
// GBufferAttachment enum; coupled to the deferred lighting compute.
layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec4 g_normal;
layout(location = 2) out vec4 g_world_pos;
layout(location = 3) out vec4 g_material;

// Forward decl; the gbuffer-pipeline composer appends either the
// visual's discard snippet or an empty stub after this fragment's body.
void velk_visual_discard();

void main()
{
    velk_visual_discard();
    g_albedo      = v_color;
    g_normal      = vec4(normalize(v_world_normal), 0.0);
    g_world_pos   = vec4(v_world_pos, 0.0);
    g_material    = vec4(0.0, 0.5, 1.0 / 255.0 /*Standard*/, 0.0);
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
layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr, OpaquePtr)
    OpaquePtr material;
};
layout(push_constant) uniform PC { GlobalData globals; DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

layout(location = 0) out vec4 frag_color;

void main()
{
    EvalContext ctx;
    ctx.data_addr   = uint64_t(root.material);
    ctx.texture_id  = root.texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_world_pos - globals.cam_pos.xyz);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_world_pos;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;
    frag_color = e.color;
}
)";

[[maybe_unused]] constexpr string_view deferred_fragment_driver_template = R"(
layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr, OpaquePtr)
    OpaquePtr material;
};
layout(push_constant) uniform PC { GlobalData globals; DrawData root; };

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 3) in vec3 v_world_pos;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) flat in uint v_shape_param;
layout(location = 6) in vec2 v_uv1;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec4 g_normal;
layout(location = 2) out vec4 g_world_pos;
layout(location = 3) out vec4 g_material;

// Composer appends either the visual's discard snippet or an empty stub.
void velk_visual_discard();

void main()
{
    velk_visual_discard();

    EvalContext ctx;
    ctx.data_addr   = uint64_t(root.material);
    ctx.texture_id  = root.texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_world_pos - globals.cam_pos.xyz);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_world_pos;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;

    vec3 N = normalize(length(e.normal) > 0.0 ? e.normal : v_world_normal);
    g_albedo    = e.color;
    g_normal    = vec4(N, 0.0);
    g_world_pos = vec4(v_world_pos, 0.0);
    g_material  = vec4(e.metallic, e.roughness, float(e.lighting_mode) / 255.0, 0.0);
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
                           "#define VELK_RASTER 1\n"
                           "#include \"velk.glsl\"\n"
                           "#include \"velk-ui.glsl\"\n"));
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
