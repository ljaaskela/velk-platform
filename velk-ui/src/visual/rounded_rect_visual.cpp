#include "rounded_rect_visual.h"

#include <velk/api/state.h>
#include <velk/hash.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

namespace {

constexpr uint64_t kPipelineKey = make_hash64("RoundedRectVisual");

// RT shape-intersect snippet: SDF-clipped rounded rect. Matches the
// signature expected by the composed intersect_shape dispatcher.
// shape.params[0] carries the corner radius set by scene_collector.
constexpr string_view kIntersectSrc = R"(
bool velk_intersect_rounded_rect(Ray ray, RtShape shape, out RayHit hit)
{
    if (!intersect_rect(ray, shape, hit)) return false;
    float radius = shape.params.x;
    if (radius <= 0.0) return true;
    float u_len = length(shape.u_axis.xyz);
    float v_len = length(shape.v_axis.xyz);
    vec2 size_w = vec2(u_len, v_len);
    vec2 p_w = hit.uv * size_w;
    vec2 half_size = size_w * 0.5;
    vec2 centered = p_w - half_size;
    vec2 d = abs(centered) - half_size + radius;
    float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
    return sdf <= 0.0;
}
)";

constexpr string_view kFragmentSrc = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local_uv;
layout(location = 2) flat in vec2 v_size;
layout(location = 0) out vec4 frag_color;

float rounded_rect_sdf(vec2 p, vec2 half_size, float radius)
{
    vec2 d = abs(p) - half_size + radius;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
}

void main()
{
    float radius = min(min(v_size.x, v_size.y) * 0.5, 12.0);
    vec2 half_size = v_size * 0.5;
    vec2 p = (v_local_uv - 0.5) * v_size;

    float dist = rounded_rect_sdf(p, half_size, radius);
    float alpha = 1.0 - smoothstep(-0.5, 0.5, dist);

    if (alpha < 0.001) discard;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
)";

} // namespace

vector<DrawEntry> RoundedRectVisual::get_draw_entries(const ::velk::size& bounds)
{
    auto state = read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = kPipelineKey;
    entry.bounds = {0, 0, bounds.width, bounds.height};
    entry.set_instance(ElementInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {0.f, 0.f, 0.f, 0.f},
        {bounds.width, bounds.height, 0.f, 0.f},
        state->color,
        {0u, 0u, 0u, 0u}});

    return {entry};
}

uint64_t RoundedRectVisual::get_raster_pipeline_key() const
{
    return kPipelineKey;
}

::velk::ShaderSource RoundedRectVisual::get_raster_source(::velk::IRasterShader::Target t) const
{
    // Forward: custom SDF fragment (vertex stays default).
    // Deferred: no override - the batch_builder composes the material's
    // deferred fragment with this visual's `velk_visual_discard` snippet
    // (see get_snippet_source below), so SDF corner clipping lands in
    // the gbuffer pass without duplicating a full fragment here.
    if (t == ::velk::IRasterShader::Target::Forward) {
        return {/*vertex*/ {}, kFragmentSrc};
    }
    return {};
}

namespace {

// Deferred discard snippet: clips the rect's rounded corners against
// the same SDF as kFragmentSrc, then early-outs via `discard`. The
// deferred compositor has no alpha blending, so this is a hard cutoff
// (no AA) — matches what every other deferred-only renderer gives you
// without MSAA/TAA.
constexpr string_view kDiscardSrc = R"(
void velk_visual_discard()
{
    float radius = min(min(v_size.x, v_size.y) * 0.5, 12.0);
    vec2 half_size = v_size * 0.5;
    vec2 p = (v_local_uv - 0.5) * v_size;
    vec2 d = abs(p) - half_size + radius;
    float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
    if (sdf > 0.0) discard;
}
)";

} // namespace

string_view RoundedRectVisual::get_shape_intersect_source() const
{
    return kIntersectSrc;
}

string_view RoundedRectVisual::get_shape_intersect_fn_name() const
{
    return "velk_intersect_rounded_rect";
}

string_view RoundedRectVisual::get_snippet_fn_name() const
{
    return "velk_visual_discard";
}

string_view RoundedRectVisual::get_snippet_source() const
{
    return kDiscardSrc;
}

} // namespace velk::ui
