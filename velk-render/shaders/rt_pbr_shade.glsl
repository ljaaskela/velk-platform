// Set to a light index to shade against that light alone (see the loop bounds
// below); -1 shades against all of them. Per-invocation, like any GLSL global.
int velk_single_light = -1;
// The reciprocal of the probability with which that light was chosen. A caller
// picking uniformly sets the light count; one importance-sampling sets
// total_weight / chosen_weight. Ignored unless velk_single_light is set.
float velk_single_light_weight = 1.0;

// Direction to a light and its distance / spot attenuation, with no shadow
// term. Shared rather than open-coded twice, so a caller weighting lights by
// their unshadowed contribution uses exactly the maths the shading loop will
// then apply.
void velk_light_sample(Light light, vec3 pos, out vec3 L, out float atten)
{
    if (light.flags.x == 3u) {
        // A square area light: one place on it, chosen evenly (velk_light_target, which its shadow
        // ray aims at), weighted so that over many its N.L times this is what the deferred
        // lighting's closed form gives (velk_area_irradiance's diffuse, times pi): the square's
        // area, its cosine toward `pos` on either face, over the distance squared.
        vec3 ln = normalize(light.direction.xyz);
        vec3 lt = (abs(ln.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), ln))
                                    : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
        vec3 lb = cross(ln, lt);
        float hs = max(light.params.w, 1e-4);
        vec2 u = rng_next_vec2() * 2.0 - 1.0;
        velk_light_target = light.position.xyz + (lt * u.x + lb * u.y) * hs;
        vec3 to_light = velk_light_target - pos;
        float dist2 = max(dot(to_light, to_light), 1e-8);
        L = to_light * inversesqrt(dist2);
        atten = 4.0 * hs * hs * abs(dot(ln, L)) / dist2;
    } else {
        velk_light_delta(light, pos, L, atten);
    }
}

// What the last shaded surface passes on of the light arriving from around it. Zero for
// materials that do not go through velk_pbr_shade. And of that, the share it passes through
// from its other side (MaterialEval::transmission).
vec3 velk_hit_diffuse = vec3(0.0);
float velk_hit_transmission = 0.0;
// Whether the surface being shaded is seen from both sides (VELK_DRAW_DOUBLE_SIDED), set by
// velk_hit_context: only such a thin sheet passes light through.
bool velk_hit_two_sided = false;
// Whether the lights' highlights count where this path is: from the camera and along mirror
// reflections, but not once it has bounced off a surface diffusely, as the deferred lighting's
// bounce light is diffuse only (and those highlights' light, the lights seen in a gloss from a
// diffuse surface, would only sparkle at a reference's sample counts).
bool velk_shade_highlights = true;

BrdfSample velk_pbr_shade(MaterialEval eval, EvalContext ctx)
{
    vec3 N = normalize(eval.normal);
    vec3 V = normalize(-ctx.ray_dir);
    float metallic  = clamp(eval.metallic, 0.0, 1.0);
    float roughness = clamp(eval.roughness, 0.04, 1.0);
    vec3 base = eval.color.rgb;
    float ao = clamp(eval.occlusion, 0.0, 1.0);
    float transmission = velk_hit_two_sided ? clamp(eval.transmission, 0.0, 1.0) : 0.0;

    // KHR_materials_specular, as the deferred lighting's.
    float F90;
    vec3 F0 = velk_specular_f0(base, metallic, eval.specular_color_factor, eval.specular_factor, F90);
    float VdotN = max(dot(V, N), 0.0);
    vec3 F = fresnel_schlick(VdotN, F0, F90);

    // Direct lighting from scene lights. Each light contributes a
    // Lambertian diffuse term scaled by its distance / spot attenuation
    // and modulated by its shadow technique's visibility.
    vec3 direct = vec3(0.0);
    // Single-light mode. -1 (the default) shades against every light, which is
    // what primary RT rays want. The reference path sets one index and the
    // reciprocal of the probability it was chosen with: its hits accumulate over
    // many frames, so one light per hit converges to the same answer while
    // keeping the ray count per hit independent of how many lights there are.
    uint li_begin = (velk_single_light >= 0) ? uint(velk_single_light) : 0u;
    uint li_end   = (velk_single_light >= 0) ? uint(velk_single_light) + 1u : pc.light_count;
    float li_weight = (velk_single_light >= 0) ? velk_single_light_weight : 1.0;
    for (uint li = li_begin; li < li_end; ++li) {
        Light light = velk_light_at(pc.lights_base + li);
        vec3 L;
        float atten;
        velk_light_sample(light, ctx.rel_pos, L, atten);
        // A thin sheet passing `transmission` of its diffuse light takes that share from
        // behind it, its shadow ray leaving the side the light is on.
        float facing = dot(N, L);
        float lobe = facing > 0.0 ? (1.0 - transmission) * facing : transmission * -facing;
        if ((lobe <= 0.0 && facing <= 0.0) || atten <= 0.0) continue;
        float shadow = velk_eval_shadow(light.flags.y, li, ctx.rel_pos, facing > 0.0 ? N : -N);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;
        direct += base * (1.0 - metallic) * lobe * radiance * li_weight;
        // Its highlight, on the side the light is on, as the deferred lighting's: GGX, Smith and
        // Schlick, times pi, since a light's intensity is the irradiance it gives over pi.
        // Continuing rays never meet a light, so this is its only highlight.
        if (facing > 0.0 && velk_shade_highlights) {
            vec3 Hl = normalize(L + V);
            float NdotH = max(dot(N, Hl), 0.0);
            float VdotH = max(dot(V, Hl), 0.0);
            float a2 = roughness * roughness * roughness * roughness;
            float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
            float D = a2 / max(3.14159265 * dd * dd, 1e-6);
            float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
            float G = VdotN / max(VdotN * (1.0 - k) + k, 1e-6) * facing / max(facing * (1.0 - k) + k, 1e-6);
            vec3 Fl = fresnel_schlick(VdotH, F0, F90);
            direct += D * G * Fl / max(4.0 * VdotN * facing, 1e-6) * facing * radiance * li_weight * 3.14159265;
        }
    }

    // Diffuse term: crude "irradiance at the normal" via a single env
    // sample, modulated by AO. Upgrade to preconvolved irradiance or
    // stochastic cosine sampling when visibly needed.
    vec3 env_at_normal = transmission > 0.0 ? mix(env_miss_color(N), env_miss_color(-N), transmission) : env_miss_color(N);
    vec3 diffuse = base * (1.0 - metallic) * env_at_normal * ao;
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    velk_hit_diffuse = kD * base * ao;
    velk_hit_transmission = transmission;

    // Specular: sample a GGX half-vector, reflect V around it. Main's
    // iterative loop evaluates the reflected ray and multiplies by F.
    vec3 H = ggx_sample_half(N, roughness, rng_next_vec2());
    vec3 Lr = reflect(-V, H);

    BrdfSample bs;
    bs.emission = vec4(kD * diffuse + direct + eval.emissive, eval.color.a);
    bs.throughput = F;
    bs.next_dir = Lr;
    bs.terminate = false;
    // Sample count scales with GGX lobe width: 1 at mirror, up to 16 at
    // fully rough. Tracer clamps against its own per-bounce cap.
    bs.sample_count_hint = uint(1.0 + roughness * roughness * 15.0);
    return bs;
}
