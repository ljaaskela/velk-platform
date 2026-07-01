#include "technique/rt_shadow.h"

namespace velk::impl {

namespace {
// One occlusion ray against the shared scene shape buffer. Uses the
// prelude's `trace_closest_hit` + `Light` struct; the compositor drops
// this snippet into the RT compute shader only when at least one
// light's shadow_tech_id resolves to this technique.
constexpr string_view rt_shadow_snippet = R"(
// Integer hash -> float in [0,1). Seeds a per-world-point per-frame disk
// sample for soft-shadow jitter; the lighting path's temporal denoiser
// integrates the resulting single sample across frames.
float velk_shadow_hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x) * (1.0 / 4294967296.0);
}

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

    // Soft shadows: jitter the ray within the light's apparent size
    // (Light.params.w). One sample per light per frame; the lighting
    // path accumulates + denoises across frames. params.w is an angular
    // radius (rad) for a directional sun, or a world-space radius for a
    // point / spot source -> its apparent angular spread is radius/dist.
    // size == 0 keeps a single hard ray (today's behaviour).
    float size = max(light.params.w, 0.0);
    if (size > 0.0) {
        float spread = (light.flags.x == 0u) ? tan(size) : (size / max(t_max, 1e-4));
        uvec3 q = floatBitsToUint(world_pos);
        uint seed = (q.x * 1973u + q.y * 9277u + q.z * 26699u
                     + VELK_GLOBALS.present_counter * 53891u) | 1u;
        vec3 up = abs(L.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 T  = normalize(cross(up, L));
        vec3 B  = cross(L, T);
        float rr = sqrt(velk_shadow_hash(seed)) * spread;
        float th = 6.28318530718 * velk_shadow_hash(seed ^ 0x9e3779b9u);
        L = normalize(L + T * (rr * cos(th)) + B * (rr * sin(th)));
    }

    // Bias along L (toward the light) rather than the surface normal:
    // double-sided thin geometry (awnings, banners) can have its
    // visible-face normal pointing away from the light, in which case
    // a normal-bias would push the origin into the back face and
    // self-shadow. L-biasing always moves into the lit half-space.
    // Magnitude tuned for meter-scale scenes; pixel-scale UIs would
    // want a per-camera or per-light bias.
    Ray r;
    r.origin = world_pos + L * 0.005;
    r.dir    = L;
    // Trim t_max so the ray doesn't hit the light's own fixture mesh.
    t_max = max(t_max - 0.005, 0.0);

    return trace_any_hit(r, t_max) ? 0.0 : 1.0;
}
)";
} // namespace

string_view RtShadow::get_source(string_view role) const
{
    if (role == ::velk::shader_role::kShadow) return rt_shadow_snippet;
    return {};
}

string_view RtShadow::get_fn_name(string_view role) const
{
    if (role == ::velk::shader_role::kShadow) return "velk_shadow_rt";
    return {};
}

} // namespace velk::impl
