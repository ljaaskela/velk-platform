#include "rounded_rect_visual.h"

#include <velk/api/state.h>
#include <velk/hash.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

namespace {

constexpr uint64_t kPipelineKey = make_hash64("RoundedRectVisual");

// Ray-intersect snippet for the compute ray tracer. Provisional signature:
// the ray tracer composes this into a template that declares the Ray and
// Hit types plus any common helpers. Function name is the convention the
// generator will splice in via a macro or include.
constexpr string_view kIntersectSrc = R"(
bool velk_intersect(Ray ray, vec2 size, out Hit hit)
{
    if (abs(ray.direction.z) < 1e-6) return false;
    float t = -ray.origin.z / ray.direction.z;
    if (t < ray.tmin || t > ray.tmax) return false;

    vec2 p = ray.origin.xy + t * ray.direction.xy;

    float radius = min(min(size.x, size.y) * 0.5, 12.0);
    vec2 half_size = size * 0.5;
    vec2 centered = p - half_size;
    vec2 d = abs(centered) - half_size + radius;
    float sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
    if (sdf > 0.0) return false;

    hit.t = t;
    hit.uv = p / size;
    hit.normal = vec3(0.0, 0.0, ray.direction.z > 0.0 ? -1.0 : 1.0);
    return true;
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

vector<DrawEntry> RoundedRectVisual::get_draw_entries(const rect& bounds)
{
    auto state = read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = kPipelineKey;
    entry.bounds = bounds;
    entry.set_instance(RectInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {bounds.x, bounds.y},
        {bounds.width, bounds.height},
        state->color});

    return {entry};
}

uint64_t RoundedRectVisual::get_pipeline_key() const
{
    return kPipelineKey;
}

string_view RoundedRectVisual::get_fragment_src() const
{
    return kFragmentSrc;
}

string_view RoundedRectVisual::get_intersect_src() const
{
    return kIntersectSrc;
}

} // namespace velk::ui
