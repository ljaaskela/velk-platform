// How much `light` could bring a place at `at`, as a luminance, whichever way the place faces.
float velk_light_strength(Light light, vec3 at)
{
    float bright = dot(light.color_intensity.rgb, vec3(0.2126, 0.7152, 0.0722)) * light.color_intensity.a;
    if (light.flags.x == 0u) return bright;
    vec3 to = light.position.xyz - at;
    float dist = length(to);
    float range = light.params.x;
    if (range > 0.0 && dist >= range) return 0.0;
    if (light.flags.x == 2u && dot(-to / max(dist, 1e-6), light.direction.xyz) <= light.params.z) return 0.0;
    float t = range > 0.0 ? 1.0 - dist / range : 1.0 / (1.0 + dist * dist);
    return bright * t * t;
}

// The strongest light velk_light_slots_at last left out, and the strongest it took, for the log.
float g_slots_left_out = 0.0;
float g_slots_strongest = 0.0;

// The lights that could bring the place at `at` the most, strongest first, as light indices
// from pc.lights_base, VELK_LIGHT_NONE in the slots left over.
void velk_light_slots_at(vec3 at, out uint slots[VELK_LIGHT_SLOTS])
{
    float most[VELK_LIGHT_SLOTS];
    for (uint s = 0u; s < VELK_LIGHT_SLOTS; ++s) {
        slots[s] = VELK_LIGHT_NONE;
        most[s] = 0.0;
    }
    g_slots_left_out = 0.0;
    uint first;
    uint candidates = velk_light_grid_count(at + VELK_GLOBALS.cam_pos.xyz, first);
    for (uint c = 0u; c < candidates; ++c) {
        uint i = velk_light_grid_at(first, c);
        float strength = velk_light_strength(velk_light_at(pc.lights_base + i), at);
        if (strength <= most[VELK_LIGHT_SLOTS - 1u]) {
            g_slots_left_out = max(g_slots_left_out, strength);
            continue;
        }
        g_slots_left_out = max(g_slots_left_out, most[VELK_LIGHT_SLOTS - 1u]);
        uint s = VELK_LIGHT_SLOTS - 1u;
        while (s > 0u && most[s - 1u] < strength) {
            most[s] = most[s - 1u];
            slots[s] = slots[s - 1u];
            --s;
        }
        most[s] = strength;
        slots[s] = i;
    }
    g_slots_strongest = most[0];
}
