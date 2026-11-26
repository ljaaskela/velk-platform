#ifndef VELK_RENDER_DEFAULT_SHADERS_H
#define VELK_RENDER_DEFAULT_SHADERS_H

#include <velk/string.h>
#include <velk/string_view.h>

#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/frame/light_pool.h>
#include <velk-render/frame/raster_shaders.h>
#include <velk-render/frame/ray_walk.h>

#include <cstdio>
#include <cstring>

namespace velk {

// Raster default shaders + driver templates + compose_eval_fragment
// live in <velk-render/frame/raster_shaders.h>. This file holds the
// scene-side shader bits: the velk-ui GLSL include, the deferred-lighting
// compute shader, and the RT compute prelude/main. Their text is in velk-render/shaders, each
// file built in as the macro VELK_<NAME>_GLSL that its generated header
// <velk-render/glsl/<name>.inc> defines.

// Rays and the built-in shape intersects, shared by every compute shader that
// traces. Registered as the "velk_intersect.glsl" include. The including shader
// declares RtShape through velk.glsl.
#include <velk-render/glsl/intersect.inc>
[[maybe_unused]] constexpr string_view velk_intersect_src = VELK_INTERSECT_GLSL;

/**
 * Fills the G-buffer from the visibility buffer: for each pixel, finds its triangle, where
 * the view ray meets it, and what the material gives there; and the pixel's record, its ids
 * and weights in its triangle (velk_pixel_ids, velk_pixel_weights). Goes after
 * compose_rt_compute's shared body, whose push constants it borrows.
 */
#include <velk-render/glsl/visibility_resolve_main.inc>
inline constexpr string_view visibility_resolve_main_src = VELK_VISIBILITY_RESOLVE_MAIN_GLSL;

/// Draws the edges of the view of the camera that chooses what a view draws (ICamera::choose_as)
/// over that view's image: its four sides out to `reach` world units, and the rectangle
/// there. A thread per point along an edge, kChooserEdgeSteps of them per edge, 8 edges.
inline constexpr uint32_t kChooserEdgeSteps = 4096;
#include <velk-render/glsl/chooser_frustum_compute.inc>
inline constexpr string_view chooser_frustum_compute_src = VELK_CHOOSER_FRUSTUM_COMPUTE_GLSL;

/// Draws the bounds of a visual's partitions over a view's image (DebugView::Partition): red for
/// a partition that chooses its levels of detail again this frame, cyan for one that keeps
/// them. `table_word` is the visual's partition table (kCullEntryWords a partition), `cut_word`
/// its kept cut in this view (kCutHeaderWords). A thread per point along an edge,
/// kPartitionEdgeSteps of them per edge, 12 edges a partition.
inline constexpr uint32_t kPartitionEdgeSteps = 256;
#include <velk-render/glsl/partition_bounds_compute.inc>
inline constexpr string_view partition_bounds_compute_src = VELK_PARTITION_BOUNDS_COMPUTE_GLSL;

/// Draws the boxes of one level of the trees over a kept cut's partitions (DebugView::RayTree)
/// over a view's image: at `level` k the children of the nodes of height k + 1, at 0 leaves of
/// four records, and a partition's root where its tree is not that tall, coloured by level. `cut_word` is
/// the kept cut (kCutHeaderWords). With `top_word` set instead, the view's top tree over the
/// partitions (kRayTopHeaderWords, DebugView::RayTop): at 0 each partition's box. A thread per
/// point along an edge, kRayTreeEdgeSteps of them per edge, 12 edges a box, two boxes a node.
inline constexpr uint32_t kRayTreeEdgeSteps = 32;
#include <velk-render/glsl/ray_tree_bounds_compute.inc>
inline constexpr string_view ray_tree_bounds_compute_src = VELK_RAY_TREE_BOUNDS_COMPUTE_GLSL;

/// Shows the visibility buffer as flat colours: mode 1 per draw, 2 per instance, 3 per
/// triangle. Mode 4 shows the depth pyramid's fourth level.
/// Modes 5 and 6 show what compute drew and how large triangles are on screen
/// (triangle_size_color). Modes 7 to 9 show level of detail clusters (cluster_color): one
/// colour per cluster of each instance, by level, and by width on screen as the camera that
/// chooses sees it (FrameGlobals at chooser_globals). Modes 10 to 12 show instances dimmed,
/// for partition_bounds_compute_src and ray_tree_bounds_compute_src to draw over. With `dim_outside`, clusters outside the
/// chooser's view are dimmed. The numbers are DebugView's.
#include <velk-render/glsl/visibility_debug_compute.inc>
inline constexpr string_view visibility_debug_compute_src = VELK_VISIBILITY_DEBUG_COMPUTE_GLSL;

// What the points of light's shaders take of the light code, built on the deferred prelude: the
// culling arena (written by the trace), the light grid, whose word is in the light header at
// CELLS_HEADER (pc.cells_header), word 18, and the area lights' axes.
#include <velk-render/glsl/light_grid_only.inc>

// Square area lights' shape, for the points of light's shaders and the probes' hits. Its source
// is velk-render/shaders/light_area.glsl; this defines VELK_LIGHT_AREA_GLSL.
#include <velk-render/glsl/light_area.inc>

// The softening of shadows, for the soft shadows' passes, and the soft shadows' words the
// lighting reads (velk-render/shaders/shadow_soft.glsl, VELK_SHADOW_SOFT_GLSL;
// shadow_apply.glsl, VELK_SHADOW_APPLY_GLSL; shadow_pack.glsl, shadow_tiles.glsl and
// shadow_soften.glsl, the soft shadows' passes).
#include <velk-render/glsl/shadow_soft.inc>
#include <velk-render/glsl/shadow_apply.inc>

// The lights by where they reach, rebuilt each frame (DeferredPath::build_light_grid), in the
// culling arena at the light header's word 18, 0xFFFFFFFF if there is none. Its words: how many
// buckets, a power of two; the size of its cubes, a float; how many lights are listed
// everywhere, and a spare word; those lights; then two words a bucket, where its lights start
// from the grid's first word and how many; then the buckets' lights. A light is listed in the
// cubes its range's box covers, or everywhere. A bucket holds the lights of the cubes that hash
// to it, so it can list lights that do not reach a place in its cube. Follows the declaration
// of velk_light_cell_words and CELLS_HEADER.
#include <velk-render/glsl/light_grid.inc>

/// The light code the points of light's passes are built with (the lighting pass and
/// light trace's, VELK_LIGHT_TRACE_MAIN_GLSL): see VELK_LIGHT_GRID_ONLY_GLSL.
[[maybe_unused]] constexpr string_view light_grid_src = VELK_LIGHT_GRID_ONLY_GLSL VELK_LIGHT_AREA_GLSL VELK_LIGHT_GRID_GLSL;

// The light terms the deferred lighting, the path tracer and the probes' hits share: GGX, Smith,
// Schlick, a material's reflectance (velk_specular_f0) and a light's way and reach
// (velk_light_delta). Its source is velk-render/shaders/light_eval.glsl; this defines
// VELK_LIGHT_EVAL_GLSL. Follows a definition of Light.
#include <velk-render/glsl/light_eval.inc>

// What lighting a surface takes, for the deferred lighting and the blended surfaces' fragments:
// the scene's lights, the environment, the highlight's terms. Follows VELK_GLOBALS and push
// constants `pc` with env_texture_id and env_params.
#include <velk-render/glsl/surface_light.inc>

// Each light's diffuse light and highlight on a surface, for the deferred lighting and the
// blended surfaces' fragments. Follows VELK_SURFACE_LIGHT_GLSL and velk_area_irradiance, with
// push constants `pc` with light_count, lights_base and ltc_magnitude_id.
#include <velk-render/glsl/surface_lights.inc>


// Default compute shader for the deferred lighting pass. Samples the
// G-buffer attachments + light buffer and writes the shaded color to
// the per-view output storage image.
//
// Shader-side responsibilities:
//   - Unlit path: pass albedo through unchanged.
//   - Standard path: evaluate Lambertian diffuse from each analytic
//     light (directional / point / spot) with distance + cone falloff.
//   - Shadow modulation lands in a later slice (B.3.d) alongside the
//     shared `velk_eval_shadow` composer.
#include <velk-render/glsl/deferred_compute_prelude.inc>
[[maybe_unused]] constexpr string_view deferred_compute_prelude_src = VELK_DEFERRED_COMPUTE_PRELUDE_GLSL;

// Area lights in closed form, for deferred_lighting_main_src. Appended after the prelude,
// whose Light declaration it depends on.
#include <velk-render/glsl/deferred_area_light.inc>
[[maybe_unused]] constexpr string_view deferred_area_light_src = VELK_DEFERRED_AREA_LIGHT_GLSL;

// Stands in for the analytic area-light path in views that have no area
// lights. The lighting main still calls velk_area_irradiance, but a constant
// zero folds away completely, leaving none of the polygon machinery's register
// footprint behind. Compiling the real thing everywhere cost bistro, which has
// no area lights at all, roughly 3x on deferred.lighting: the code never ran,
// but the registers its arrays reserve are charged to every branch.
#include <velk-render/glsl/deferred_area_light_stub.inc>
[[maybe_unused]] constexpr string_view deferred_area_light_stub_src = VELK_DEFERRED_AREA_LIGHT_STUB_GLSL;

/**
 * The deferred path's driver for blended surfaces' fragments (the transparent pass), composed with
 * a material's eval (compose_eval_fragment): lit as the deferred lighting lights a pixel, by the
 * points of light their record keeps (velk_light_direct, which blended surfaces' fragments list,
 * light_claim_blended_src), else every light unshadowed; indirect light from what their record
 * keeps, else the environment; with the Fresnel boosted opacity of glass. Its head, then the area
 * lights' code (deferred_area_light_src or its stub), then transparent_lit_main_src. Its push
 * constants are the draw's, then the lighting's (TransparentLitPushC).
 */
#include <velk-render/glsl/transparent_lit_head.inc>
[[maybe_unused]] constexpr string_view transparent_lit_head_src = VELK_TRANSPARENT_LIT_HEAD_GLSL VELK_SURFACE_LIGHT_GLSL VELK_LIGHT_GRID_GLSL VELK_LIGHT_POOL_GLSL VELK_LIGHT_SLOTS_GLSL VELK_LIGHT_READ_GLSL
    VELK_FRAGMENT_TRIANGLE_GLSL;

#include <velk-render/glsl/transparent_lit_main.inc>
[[maybe_unused]] constexpr string_view transparent_lit_main_src = VELK_SURFACE_LIGHTS_GLSL VELK_TRANSPARENT_LIT_MAIN_GLSL;

#include <velk-render/glsl/deferred_lighting_main.inc>
[[maybe_unused]] constexpr string_view deferred_lighting_main_src = VELK_DEFERRED_LIGHTING_MAIN_GLSL;

// The soft shadows from the points of light (DeferredPath::emit_shadow_soft_passes), three passes
// built on the deferred prelude with the light grid: what each pixel's lights with a size are
// (shadow_pack.glsl), the tiles near an edge to soften (shadow_tiles.glsl), and the softening of
// those (shadow_soften.glsl), which the lighting then takes (velk_shadow_apply).
#include <velk-render/glsl/shadow_pack.inc>
[[maybe_unused]] constexpr string_view shadow_pack_main_src = VELK_SHADOW_PACK_GLSL;
#include <velk-render/glsl/shadow_tiles.inc>
[[maybe_unused]] constexpr string_view shadow_tiles_main_src = VELK_SHADOW_TILES_GLSL;
#include <velk-render/glsl/shadow_soften.inc>
[[maybe_unused]] constexpr string_view shadow_soften_main_src = VELK_SHADOW_SOFTEN_GLSL;

// Compute ray tracer prelude. Fixed header that precedes all material
// snippets. Declares extensions, storage-image binding, shape struct,
// push constants, intersect_rect, and the full stochastic-RT toolkit
// (RNG, GGX sampling, trace_ray). velk_resolve_fill is forward-declared
// so trace_ray can call it; the composer generates its definition after
// all material snippets and before main.
#include <velk-render/glsl/rt_compute_prelude.inc>
[[maybe_unused]] constexpr string_view rt_compute_prelude_src = VELK_RT_COMPUTE_PRELUDE_GLSL;

// Shared PBR shading helper. Converts a MaterialEval produced by a
// material's velk_eval_<name> into a BrdfSample. Every Lit material
// routes through this instead of open-coding its own PBR body.
//
// Placement: composer emits this string between `velk_eval_shadow` and
// `velk_resolve_fill` so it can call the former and be called by the
// latter. Depends on pc.lights, pc.light_count, env_miss_color,
// velk_eval_shadow, ggx_sample_half, rng_next_vec2 — all in scope at
// that point.
#include <velk-render/glsl/rt_pbr_shade.inc>
[[maybe_unused]] constexpr string_view rt_pbr_shade_src = VELK_LIGHT_EVAL_GLSL VELK_RT_PBR_SHADE_GLSL;

// Compute ray tracer main body.
//
// Primary visibility runs the classic painter's loop (iterate all shapes
// in CPU-sorted depth order, composite each with its alpha over the
// accumulator) so 2D UI with stacked semi-transparent cards matches the
// rasterizer. Each primary hit evaluates its material; if the material
// returns a bounce (terminate == false, i.e. StandardMaterial), we run a
// small iterative secondary loop for that hit's specular path. GLSL has
// no recursion, so bounces can't be a function — they live inline here.
#include <velk-render/glsl/rt_compute_main.inc>
[[maybe_unused]] constexpr string_view rt_compute_main_src = VELK_RT_COMPUTE_MAIN_GLSL;

/// Path tracing for reference images. Goes after compose_rt_compute's shared body. Each frame
/// traces one path for a share of the pixels and adds it to a running sum, so the cost of a
/// frame stays bounded. `extras` is the sum image (RGBA32F, alpha counts paths), the image
/// shown (RGBA16F), width and height. `shapes_base` is nonzero to start over. `env.y` is the
/// environment texture, and `env.z` and `env.w` its intensity and rotation as bits. `env.x`
/// must be 0, so that a surface is not lit by the environment along its normal as well.
#include <velk-render/glsl/reference_path.inc>

#include <velk-render/glsl/rt_reference_main.inc>
inline constexpr string_view rt_reference_main_src = VELK_REFERENCE_PATH_GLSL VELK_RT_REFERENCE_MAIN_GLSL;

/// Measures a frame for render tests. Mode 0 clears this frame's words. Mode 1 follows each
/// surface back to the previous frame and adds up how much its lighting changed, so camera
/// motion does not count and surfaces that moved are skipped. `words` holds: valid, summed
/// change times 256, over 5%, over 20%, the same four for surfaces facing up or down, two
/// unused, then per screen tile valid and summed.
#include <velk-render/glsl/test_diagnostics_compute.inc>
inline constexpr string_view test_diagnostics_compute_src = VELK_TEST_DIAGNOSTICS_COMPUTE_GLSL;

/// Appends the #include lines of the intersect snippets registered for this frame.
inline void append_rt_intersect_includes(string& src, const IFrameSnippetRegistry& snippets)
{
    const auto& intersect_info = snippets.intersect_info_by_id();
    for (auto id : snippets.frame_intersects()) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }
}

/// Appends intersect_shape: built-in rect/cube/sphere kinds and registered visual kinds (3+).
/// Triangle meshes are walked by velk_walk and never get here.
inline void append_rt_intersect_shape(string& src, const IFrameSnippetRegistry& snippets)
{
    const auto append_literal = [&src](const char* s) { src += string_view(s, std::strlen(s)); };
    const auto& intersect_info = snippets.intersect_info_by_id();
    char buf[128];
    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    for (auto id : snippets.frame_intersects()) {
        if (id < 3 || id - 3 >= intersect_info.size()) continue;
        const auto& ii = intersect_info[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");
}

/**
 * @brief Composes a compute shader that traces rays and runs no material code: the RT prelude,
 *        the frame's intersect snippets and intersect_shape, then @p main_src. Its pipeline
 *        depends only on the frame's intersects. A work group is @p group_threads threads in a
 *        row; 0 keeps the prelude's 8 by 8.
 */
inline string compose_rt_trace(const IFrameSnippetRegistry& snippets, string_view main_src, uint32_t group_threads = 0)
{
    string src;
    if (group_threads != 0) {
        // The group size goes right after the #version line, before the prelude reads it.
        const size_t version = rt_compute_prelude_src.find(string_view("#version"));
        const size_t line_end = rt_compute_prelude_src.find('\n', version);
        src += rt_compute_prelude_src.substr(0, line_end + 1);
        char define[64];
        const int length = std::snprintf(define, sizeof(define), "#define VELK_RT_GROUP_THREADS %u\n", group_threads);
        // As long as written: a string_view of the whole array would take its NUL and what follows.
        src += string_view(define, length > 0 ? size_t(length) : 0u);
        src += rt_compute_prelude_src.substr(line_end + 1);
    } else {
        src += rt_compute_prelude_src;
    }
    append_rt_intersect_includes(src, snippets);
    append_rt_intersect_shape(src, snippets);
    src += main_src;
    return src;
}

/**
 * A point on a drawn triangle, the same for a pixel and a ray: from the draw (VelkDrawData),
 * the instance, the cluster and the triangle, then the weights of its corners, what its
 * material is given. Part of compose_rt_compute's shared body, after velk_material_varyings.
 */
#include <velk-render/glsl/surface.inc>
inline constexpr string_view velk_surface_src = VELK_SURFACE_GLSL;

/**
 * @brief Composes the full RT compute pipeline source from registered
 *        material / shadow / intersect snippets.
 *
 * RT-side equivalent of compose_eval_fragment. Unlike the raster driver
 * templates (which substitute a single material's eval function name
 * via `<%EVAL_FN%>`), the RT pipeline dispatches to every active
 * material in a switch — so the composer is a procedural builder rather
 * than a placeholder template. Sections, in order:
 *
 *   1. rt_compute_prelude_src                (shared prelude)
 *   2. material #include lines               (one per active material)
 *   3. shadow tech #include lines
 *   4. intersect #include lines
 *   5. velk_eval_shadow switch               (calls registered shadow snippets)
 *   6. rt_pbr_shade_src                      (uses velk_eval_shadow)
 *   7. velk_resolve_fill switch              (calls eval_fn, may call velk_pbr_shade)
 *   8. intersect_shape switch                (built-in kinds + registered snippets)
 *   9. rt_compute_main_src                   (primary loop + bounce logic)
 */
inline string compose_rt_compute(const IFrameSnippetRegistry& snippets,
                                 string_view main_src = {})
{
    const auto& material_ids        = snippets.frame_materials();
    const auto& shadow_tech_ids     = snippets.frame_shadow_techs();
    const auto& material_info       = snippets.material_info_by_id();
    const auto& shadow_tech_info    = snippets.shadow_tech_info_by_id();

    string src;
    src += rt_compute_prelude_src;
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        src += string_view("#include \"", 10);
        src += mi.include_name;
        src += string_view("\"\n", 2);
    }
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }
    append_rt_intersect_includes(src, snippets);

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    char buf[128];

    // velk_eval_shadow: dispatch by shadow_tech id.
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

    // Shared PBR shading helper — defined after velk_eval_shadow (which
    // it calls) and before velk_resolve_fill (which calls it for Lit
    // materials).
    src += rt_pbr_shade_src;

    // velk_material_varyings: dispatch by material id to its vertex function.
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size() || material_info[id - 1].vertex_fn.empty()) continue;
        append_literal("#include \"");
        src += material_info[id - 1].vertex_include;
        append_literal("\"\n");
    }
    append_literal("VelkVaryings velk_material_varyings(uint mid, VelkVertex3D v, ElementInstance inst, GlobalData g) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size() || material_info[id - 1].vertex_fn.empty()) continue;
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += material_info[id - 1].vertex_fn;
        append_literal("(v, inst, g);\n");
    }
    append_literal("        default: return velk_default_varyings(v, inst, g);\n");
    append_literal("    }\n");
    append_literal("}\n");
    src += velk_surface_src;

    // velk_eval_material: dispatch by material id, without shading.
    append_literal("MaterialEval velk_eval_material(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);\n");
    }
    append_literal("        default: {"
                   " MaterialEval e = velk_default_material_eval();"
                   " e.color = ctx.base;"
                   " e.normal = ctx.normal;"
                   " e.roughness = 0.5;"
                   " e.lighting_mode = VELK_LIGHTING_STANDARD;"
                   " return e; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    // velk_resolve_fill: dispatch by material id; route Lit through
    // velk_pbr_shade and Unlit straight to emission.
    append_literal("BrdfSample velk_resolve_fill(uint mid, EvalContext ctx) {\n");
    append_literal("    switch (mid) {\n");
    for (auto id : material_ids) {
        if (id == 0 || id > material_info.size()) continue;
        const auto& mi = material_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: { MaterialEval e = ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += mi.fn_name;
        append_literal("(ctx);"
                       " if (e.lighting_mode == VELK_LIGHTING_STANDARD)"
                       " return velk_pbr_shade(e, ctx);"
                       " BrdfSample bs;"
                       " bs.emission = e.color;"
                       " bs.throughput = vec3(0.0);"
                       " bs.next_dir = vec3(0.0);"
                       " bs.terminate = true;"
                       " bs.sample_count_hint = 1u;"
                       " return bs; }\n");
    }
    append_literal("        default: { BrdfSample bs; bs.emission = ctx.base; bs.throughput = vec3(0.0); bs.next_dir = vec3(0.0); bs.terminate = true; bs.sample_count_hint = 1u; return bs; }\n");
    append_literal("    }\n");
    append_literal("}\n");

    append_rt_intersect_shape(src, snippets);

    // Everything above is the shared RT body. Callers wanting that machinery
    // under a different entry point (the light probes pass) supply their own main.
    src += main_src.empty() ? rt_compute_main_src : main_src;
    return src;
}

} // namespace velk

#endif // VELK_RENDER_DEFAULT_SHADERS_H
