// Light `i`'s diffuse light and highlight, `through` of it getting through, added to the sums.
// As velk_surface_lights says.
void velk_surface_light(uint i, float through, vec3 world_pos, vec3 N, vec3 V, vec3 F0, float F90, float roughness,
                        float stand_in, float transmission, inout vec3 direct, inout vec3 highlights, inout vec3 area_irr,
                        inout vec3 area_spec)
{
    float NdotV = max(dot(N, V), 0.0);
    float a = roughness * roughness;
    // A stand-in's face, turned to the viewer.
    vec3 face = dot(N, V) < 0.0 ? -N : N;
    {
        Light light = velk_light_at(pc.lights_base + i);
        // Area lights are integrated in closed form, and blocked by what the points say.
        if (light.flags.x == 3u) {
            vec2 es = velk_area_irradiance(light, world_pos, N, V, roughness);
            vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * through;
            // pi converts clamped-cosine units to the same scale as a delta light's N.L, so
            // authored intensities stay comparable.
            float irradiance = max(es.x, 0.0) * (1.0 - transmission);
            if (transmission > 0.0) irradiance += max(velk_area_irradiance(light, world_pos, -N, V, roughness).x, 0.0) * transmission;
            if (irradiance > 0.0) area_irr += radiance * irradiance * 3.14159265;
            if (es.y > 0.0 && pc.ltc_magnitude_id != 0u) {
                // Split-sum: the fitted pair is (lobe energy, Fresnel
                // weight), applied the same way the env specular applies
                // its BRDF LUT, so both specular sources stay on one scale.
                vec2 t2 = velk_texture(pc.ltc_magnitude_id,
                                       vec2(clamp(roughness, 0.0, 1.0),
                                            sqrt(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0)))).xy;
                area_spec += radiance * es.y * (F0 * t2.x + (vec3(F90) - F0) * t2.y)
                             * 3.14159265;
            }
            return;
        }
        vec3 L;
        float atten;
        velk_light_delta(light, world_pos, L, atten);
        float NdotL = max(dot(N, L), 0.0);
        float diffuse = stand_in >= 0.0 ? mix(velk_ball_response(L, V), max(dot(face, L), 0.0), stand_in)
                                        : mix(NdotL, max(-dot(N, L), 0.0), transmission);
        if ((diffuse <= 0.0 && NdotL <= 0.0) || atten <= 0.0) return;
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten;
        direct += diffuse * radiance * through;
        if (NdotL <= 0.0) return;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);
        float D = ggx_d(NdotH, a);
        float G = smith_g(NdotV, NdotL, roughness);
        vec3 F = fresnel_schlick(VdotH, F0, F90);
        // A light's intensity is the irradiance it gives over pi (diffuse is albedo times it, as the
        // indirect light, a radiance, is), so its highlight takes pi back, as an area light's does.
        highlights += (D * G) * F / max(4.0 * NdotV * NdotL, 1e-6) * NdotL * radiance * through * 3.14159265;
    }
}

// At `world_pos` (from the camera) facing `N`, seen along `V`: through what the points of light
// keep when `points` (the lights `point_lights` and what of each gets through), else every light
// unshadowed. Area lights in closed form. A stand-in for what it is too small to draw
// (`stand_in` its flatness, velk_stand_in_flatness; below 0 for a surface as drawn) takes diffuse
// light as a rough ball would, toward its face's as it is flatter, that face turned to the viewer.
// A thin sheet passing `transmission` of its diffuse light takes that share from behind it, the
// rest from in front. Its reflectance is `F0` at normal incidence and `F90` at grazing
// (velk_specular_f0).
void velk_surface_lights(bool points, uint point_lights[VELK_LIGHT_SLOTS], float point_through[VELK_LIGHT_SLOTS],
                         vec3 world_pos, vec3 N, vec3 V, vec3 F0, float F90, float roughness, float stand_in, float transmission,
                         out vec3 direct, out vec3 highlights, out vec3 area_irr, out vec3 area_spec)
{
    direct = vec3(0.0);
    highlights = vec3(0.0);
    area_irr = vec3(0.0);
    area_spec = vec3(0.0);
    uint light_count = points ? VELK_LIGHT_SLOTS : pc.light_count;
    for (uint k = 0u; k < light_count; ++k) {
        if (points && point_lights[k] == VELK_LIGHT_NONE) continue;
        uint i = points ? point_lights[k] : k;
        velk_surface_light(i, points ? point_through[k] : 1.0, world_pos, N, V, F0, F90, roughness, stand_in, transmission, direct,
                           highlights, area_irr, area_spec);
    }
}
