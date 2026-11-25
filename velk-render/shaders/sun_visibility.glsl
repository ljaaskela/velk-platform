// The sun: the first lit directional light. VELK_LIGHT_NONE (all bits) for none.
uint velk_sun_index()
{
    for (uint i = 0u; i < pc.light_count; ++i) {
        Light light = velk_light_at(pc.lights_base + i);
        if (light.flags.x == 0u && light.color_intensity.a > 0.0) return i;
    }
    return 0xFFFFFFFFu;
}

// Whether light along `dir` reaches `pos` (from the camera) on a surface facing `n`: 1 where nothing
// is in the way, 0 where something is, between for blended surfaces on the way. One seen from both
// sides (`two_sided`) is lit on the side the light is on. A stand-in for small pieces (`stand_in`)
// faces every way: its ray starts off it toward the light, as a light point's does. `blocker_t` is
// how far away what was in the way is, 0 where nothing was; `facing_away` is true where the surface
// faces away from the light, which then is in its own way at no distance.
float velk_sun_visibility(vec3 pos, vec3 n, bool two_sided, bool stand_in, vec3 dir, out float blocker_t, out bool facing_away)
{
    blocker_t = 0.0;
    if (two_sided && dot(n, dir) < 0.0) n = -n;
    facing_away = !stand_in && dot(n, dir) <= 0.0;
    if (facing_away) return 0.0;
    Ray ray;
    ray.origin = pos + (stand_in ? dir : n) * (0.01 + length(pos) * 1e-4);
    ray.dir = dir;
    velk_ray_blocker_t = 0.0;
    float through = trace_visibility(ray, 1e30);
    if (through < 1.0) blocker_t = velk_ray_blocker_t;
    return through;
}
