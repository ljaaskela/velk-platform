// The bounce light at a pixel from the screen probes around it (light_probes_main_src, this
// frame's store at pc.spare1): the four nearest, each seen from `n` (velk_probe_eval), by how
// near they are on screen, how much their normal agrees with `n` and how near `pos` is to their
// surface's plane. `along` is the same probes' light seen from `r`, for a rough reflection along
// it, and `match` how well they matched, 0 to 1. False where none of them has a surface like it.
bool velk_probe_indirect(ivec2 coord, vec3 pos, vec3 n, vec3 r, out vec3 light, out vec3 along, out float match)
{
    light = vec3(0.0);
    along = vec3(0.0);
    match = 0.0;
    if (pc.spare1 == 0xFFFFFFFFu) return false;
    const float kProbe = 16.0;
    uint across = (pc.width + 15u) / 16u;
    uint down = (pc.height + 15u) / 16u;
    // A probe sits at the middle pixel of its square, whose centre is 8.5 pixels in.
    vec2 at = (vec2(coord) + 0.5 - (kProbe * 0.5 + 0.5)) / kProbe;
    ivec2 base = ivec2(floor(at));
    vec2 f = at - vec2(base);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 p = clamp(base + ivec2(k & 1, k >> 1), ivec2(0), ivec2(int(across) - 1, int(down) - 1));
        uint w = pc.spare1 + (uint(p.y) * across + uint(p.x)) * VELK_PROBE_WORDS;
        if (uintBitsToFloat(velk_light_cell_words.data[w + VELK_PROBE_FRAMES]) <= 0.0) continue;
        vec3 pn = velk_probe_normal(w);
        vec3 ppos = velk_probe_position(w);
        float bilinear = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
        float facing = pow(max(dot(pn, n), 0.0), 4.0);
        float plane = exp(-abs(dot(pn, pos - ppos)) / (0.02 * length(pos) + 1e-3));
        float weight = bilinear * facing * plane + 1e-6 * bilinear;
        VelkProbeLight probe = velk_probe_read(w);
        light += weight * velk_probe_eval(probe, pn, n);
        along += weight * velk_probe_eval(probe, pn, r);
        total += weight;
    }
    if (total <= 1e-5) return false;
    light /= total;
    along /= total;
    // The bilinear weights add up to one: what is left of them is how much matched.
    match = clamp(total, 0.0, 1.0);
    return true;
}

// Lights each pixel: each light's diffuse light and highlight, shadowed by the points of light
// on the cut (pc.spare3 the pool, pc.spare4 and pc.spare2 the pixel ids' word and width,
// pc.spare5 kLightPixels' bits), unshadowed until they are traced; indirect light from the
// screen probes (velk_probe_indirect), or the environment; reflections from the environment;
// area lights in closed form; emission. Writes the finished image.
void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;
    // While VELK_LIGHT_READ names pixels, the pool's debug block (VELK_POOL_DEBUG), else none: the
    // frame the lighting ran in (VELK_DEBUG_READ_STAMP), which the next frame's sweep copies with
    // this frame's counts.
    uint light_debug = pc.spare3 != VELK_LIGHT_NONE ? velk_light_cell_words.data[pc.spare3 + VELK_POOL_DEBUG] : VELK_LIGHT_NONE;
    if (coord.x == 0 && coord.y == 0 && light_debug != VELK_LIGHT_NONE) {
        velk_light_cell_words.data[light_debug + VELK_DEBUG_READ_STAMP] = VELK_GLOBALS.present_counter;
    }

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec4 albedo = velk_texture(pc.albedo_tex_id, uv);
    vec4 normal_and_error = velk_texture(pc.normal_tex_id, uv);
    vec3 world_n = normal_and_error.xyz;
    vec4 position = velk_texture(pc.worldpos_tex_id, uv);
    vec3 world_pos = position.xyz;
    vec4 mat = velk_texture(pc.material_tex_id, uv);

    // No surface: the environment along the view ray.
    if (dot(world_n, world_n) < 1e-6) {
        vec2 ndc = uv * 2.0 - 1.0;
        mat4 inv_vp = VELK_GLOBALS.inverse_view_projection;
        vec4 near_h = inv_vp * vec4(ndc, 1.0, 1.0);
        vec4 far_h = inv_vp * vec4(ndc, 0.0, 1.0);
        vec3 rd = normalize(far_h.xyz / far_h.w - near_h.xyz / near_h.w);
        imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(env_miss_color(rd), 1.0));
        return;
    }

    // Added to the lit colour, before tonemapping, so that bloom can pick up bright emitters.
    vec4 emissive_and_transmission = velk_texture(pc.emissive_tex_id, uv);
    vec3 emissive = emissive_and_transmission.rgb;
    // Of its diffuse light, the share a thin sheet passes to its other side (MaterialEval::transmission).
    float transmission = emissive_and_transmission.a;

    // LightingMode encoded in mat.b (0 = Unlit, 1 = Standard, ...).
    uint lighting_mode = uint(mat.b * 255.0 + 0.5);
    float metallic = clamp(mat.r, 0.0, 1.0);
    float roughness = clamp(mat.g, 0.04, 1.0);
    // A stand-in for what is too small to draw (velk_stand_in_flatness) reflects as roughly as
    // what it stands for faces every way: not as a smooth face seen edge on.
    float stand_in = velk_stand_in_unpack(mat.a);
    if (stand_in >= 0.0) roughness = mix(1.0, roughness, stand_in);

    vec3 color = albedo.rgb;
    if (lighting_mode != 0u) {
        vec3 N = normalize(world_n);
        vec3 V = normalize(-world_pos);
        float NdotV = max(dot(N, V), 0.0);
        // A dielectric's reflectance (velk_specular_f0), its square root at normal incidence and
        // its grazing one (the resolve's RESOLVE_SPECULAR, here pc.spare6); a metal's its colour.
        vec4 specular = velk_texture(pc.spare6, uv);
        vec3 F0 = mix(specular.rgb * specular.rgb, albedo.rgb, metallic);
        float F90 = mix(specular.a, 1.0, metallic);

        // The lights its points of light keep and what of each gets through; before its points
        // are traced, every light, unshadowed.
        uint point_lights[VELK_LIGHT_SLOTS];
        float point_through[VELK_LIGHT_SLOTS];
        bool points = false;
        uint radiance = VELK_LIGHT_NONE;
        uint radiance_dir = VELK_LIGHT_NONE;
#ifdef VELK_LIGHT_CODES
        g_light_codes_first = VELK_LIGHT_NONE;
        g_light_pending_slots = 0u;
#endif
        if (pc.spare3 != VELK_LIGHT_NONE) {
            points = velk_light_direct(pc.spare3, velk_pixel_ids(pc.spare4, pc.spare2, coord),
                                       velk_pixel_weights(pc.spare4, pc.spare2, coord), world_pos, position.w,
                                       uintBitsToFloat(pc.spare5), point_lights, point_through, radiance, radiance_dir);
        }
        // Counted, for the log: a lit pixel whose light is not traced yet.
        if (!points && pc.spare3 != VELK_LIGHT_NONE) atomicAdd(velk_light_cell_words.data[pc.spare3 + VELK_POOL_UNTRACED_PIXELS], 1u);
        // The soft shadows (their first word plus one in pc.cam_pos.w's bits, 0 for none): what of
        // each light their softening spread over its penumbra, in place of what the points keep.
        uint shadow_base = floatBitsToUint(pc.cam_pos.w);
        if (points && shadow_base != 0u) velk_shadow_apply(shadow_base - 1u, coord, point_lights, point_through);
#ifdef VELK_LIGHT_CODES
        // For the log, while VELK_LIGHT_READ is set: this pixel's lights that bring it light and
        // whose points have not traced them yet, or a change made stale; before its points are
        // traced, every light that brings it light. Added to the debug block's PENDING.
        if (light_debug != VELK_LIGHT_NONE) {
            uint pending = 0u;
            if (points) {
                for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
                    uint i = point_lights[k];
                    if (i == VELK_LIGHT_NONE || i >= pc.light_count) continue;
                    if (velk_light_strength(velk_light_at(pc.lights_base + i), world_pos) <= 0.0) continue;
                    uint s = velk_light_codes_slot(i);
                    if (s < VELK_LIGHT_SLOTS && (g_light_pending_slots & (1u << s)) != 0u) ++pending;
                }
            } else {
                uint count = min(pc.light_count, 2048u);
                for (uint i = 0u; i < count; ++i) {
                    if (velk_light_strength(velk_light_at(pc.lights_base + i), world_pos) > 0.0) ++pending;
                }
            }
            if (pending != 0u) atomicAdd(velk_light_cell_words.data[light_debug + VELK_DEBUG_PENDING], pending);
        }
#endif

        vec3 direct;
        vec3 highlights;
        vec3 area_irr;
        vec3 area_spec;
        velk_surface_lights(points, point_lights, point_through, world_pos, N, V, F0, F90, roughness, stand_in, transmission, direct,
                            highlights, area_irr, area_spec);

        // Env lighting: single-sample approximation. Diffuse reads along N with a deep LOD
        // so it reads roughly the irradiance average; specular reads the mirror reflection
        // with LOD scaled by roughness as a cheap GGX-prefilter stand-in.
        float env_max_lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        vec3 env_diffuse = env_miss_color_lod(N, env_max_lod);
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), roughness * env_max_lod);

        // Light from other surfaces and the sky: the screen probes around it, or the environment
        // where none has a surface like it yet. The same probes seen along the reflection stand in
        // for a rough reflection, shadowed as the environment is not, as far as they matched the
        // surface.
        vec3 indirect;
        vec3 probe_specular;
        float match;
        bool probed = velk_probe_indirect(coord, world_pos, N, reflect(-V, N), indirect, probe_specular, match);
        if (probed) env_specular = mix(env_specular, probe_specular, clamp(match * 4.0, 0.0, 1.0));
        // Traced reflections (reflect_resolve_src, pc.spare0) where the surface is smooth enough
        // for them, fading into the rough reflection toward the roughness they stop at.
        if (pc.spare0 != VELK_LIGHT_NONE) {
            vec4 traced = texelFetch(velk_textures[nonuniformEXT(pc.spare0)], coord, 0);
            if (traced.a > 0.0) env_specular = mix(env_specular, traced.rgb, 1.0 - smoothstep(0.21, 0.3, roughness));
        }
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, F90, roughness);

        if (!probed) {
            indirect = env_diffuse;
        } else if (radiance != VELK_LIGHT_NONE
                   && ((uint(coord.x) & 3u) | ((uint(coord.y) & 3u) << 2u)) == (VELK_GLOBALS.present_counter * 7u & 15u)) {
            // A pixel in 16 each frame, a 4 by 4 pattern that moves every frame, keeps its record's
            // indirect light for rays that hit it off screen (velk_probe_hit_indirect): the first
            // as it is, then a little of each; and the way it faces, likewise, not made of unit
            // length again, so that a record seen facing many ways keeps a short one. Pixels of one
            // record race; any of them will do.
            uint kept = velk_light_cell_words.data[radiance];
            bool first = kept == VELK_LIGHT_NONE;
            velk_light_cell_words.data[radiance] = velk_pack_rgb9e5(first ? indirect : mix(velk_rgb9e5(kept), indirect, 0.125));
            if (radiance_dir != VELK_LIGHT_NONE) {
                vec3 way = first ? N : mix(unpackSnorm4x8(velk_light_cell_words.data[radiance_dir]).xyz, N, 0.125);
                velk_light_cell_words.data[radiance_dir] = packSnorm4x8(vec4(way, 0.0));
            }
        }

        // A thin sheet passing light through: that share of its diffuse light from its other side.
        if (transmission > 0.0) {
            vec3 back;
            vec3 back_specular;
            float back_match;
            if (!velk_probe_indirect(coord, world_pos, -N, reflect(-V, -N), back, back_specular, back_match)) {
                back = env_miss_color_lod(-N, env_max_lod);
            }
            indirect = mix(indirect, back, transmission);
        }

        color = albedo.rgb * (1.0 - metallic) * (direct + indirect + area_irr) + highlights + F_env * env_specular
              + area_spec;

        // At VELK_LIGHT_READ's pixels, its terms for the log (VELK_DEBUG_READ_TERMS).
        if (light_debug != VELK_LIGHT_NONE) {
            uint here = uint(coord.x) | (uint(coord.y) << 16u);
            for (uint k = 0u; k < 4u; ++k) {
                if (velk_light_cell_words.data[light_debug + VELK_DEBUG_READ_PIXELS + k] != here) continue;
                uint at = light_debug + VELK_DEBUG_READ_TERMS + k * VELK_LIGHT_READ_WORDS;
                vec3 terms[7] = vec3[7](albedo.rgb, direct, indirect, env_diffuse, highlights + F_env * env_specular + area_spec,
                                        area_irr, color);
                for (uint t = 0u; t < 7u; ++t) {
                    for (uint c = 0u; c < 3u; ++c) velk_light_cell_words.data[at + t * 3u + c] = floatBitsToUint(terms[t][c]);
                }
                velk_light_cell_words.data[at + VELK_LIGHT_READ_METALLIC] = floatBitsToUint(metallic);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FLAGS] = (points ? 1u : 0u) | (probed ? 2u : 0u)
                    | (VELK_GLOBALS.present_counter << 8u);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_MATCH] = floatBitsToUint(match);
                uint explain[VELK_LIGHT_EXPLAIN_WORDS];
                velk_light_explain(pc.spare3, velk_pixel_ids(pc.spare4, pc.spare2, coord), velk_pixel_weights(pc.spare4, pc.spare2, coord),
                                   world_pos, position.w, explain);
                for (uint e = 0u; e < VELK_LIGHT_EXPLAIN_WORDS; ++e) {
                    velk_light_cell_words.data[at + VELK_LIGHT_READ_EXPLAIN + e] = explain[e];
                }
                uint first_light = points ? point_lights[0] : VELK_LIGHT_NONE;
                float n_dot_l = 0.0;
                if (first_light != VELK_LIGHT_NONE) n_dot_l = dot(N, -normalize(velk_light_at(pc.lights_base + first_light).direction.xyz));
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT] = first_light;
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT + 1u] = floatBitsToUint(points ? point_through[0] : -1.0);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT + 2u] = floatBitsToUint(n_dot_l);
                velk_light_cell_words.data[at + VELK_LIGHT_READ_FIRST_LIGHT + 3u] = velk_pixel_ids(pc.spare4, pc.spare2, coord).x;
                for (uint c = 0u; c < 3u; ++c) velk_light_cell_words.data[at + VELK_LIGHT_READ_HIGHLIGHTS + c] = floatBitsToUint(highlights[c]);
                for (uint c = 0u; c < 3u; ++c) velk_light_cell_words.data[at + VELK_LIGHT_READ_NORMAL + c] = floatBitsToUint(N[c]);
            }
        }
    }

    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(color + emissive, albedo.a));
}
