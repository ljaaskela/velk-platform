#include "rt_shadow.h"

namespace velk::impl {

namespace {
// One occlusion ray against the shared scene shape buffer. Uses the
// prelude's `trace_closest_hit` + `Light` struct; the compositor drops
// this snippet into the RT compute shader only when at least one
// light's shadow_tech_id resolves to this technique.
constexpr string_view rt_shadow_snippet = R"(
float velk_shadow_rt(uint light_idx, vec3 world_pos, vec3 world_normal)
{
    Light light = pc.lights.data[light_idx];

    // Shadow ray direction + maximum distance depend on light kind.
    // Directional lights occlude anything between the surface and
    // infinity along the reverse-forward axis; point/spot lights
    // occlude only inside their light-to-surface segment.
    vec3 L;
    float t_max;
    if (light.flags.x == 0u) {
        L = -light.direction.xyz;
        t_max = 1e30;
    } else {
        vec3 to_light = light.position.xyz - world_pos;
        t_max = length(to_light);
        if (t_max < 1e-6) return 1.0;
        L = to_light / t_max;
    }

    // Bias along the surface normal, which is always unambiguously off
    // the receiving face. Biasing along L self-intersects at grazing
    // angles (cos(N, L) -> 0 leaves the origin on the surface).
    // Scene-scale: a few units tolerates float precision at km-range
    // distances without leaving a visible gap on close surfaces.
    vec3 n = normalize(world_normal);
    Ray r;
    r.origin = world_pos + n * 0.5;
    r.dir    = L;

    RayHit hit;
    if (!trace_closest_hit(r, hit)) return 1.0;
    return (hit.t >= t_max) ? 1.0 : 0.0;
}
)";
} // namespace

string_view RtShadow::get_snippet_fn_name() const
{
    return "velk_shadow_rt";
}

string_view RtShadow::get_snippet_source() const
{
    return rt_shadow_snippet;
}

} // namespace velk::impl
