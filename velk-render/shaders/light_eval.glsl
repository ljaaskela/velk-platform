// GGX normal distribution, Smith geometry, Schlick Fresnel. Same forms
// used by the RT path's velk_fill_standard.
float ggx_d(float NdotH, float a)
{
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265358979323846 * denom * denom, 1e-6);
}

float smith_g1(float NdotX, float a)
{
    float k = (a + 1.0); k = k * k * 0.125; // (a+1)^2 / 8, Schlick-GGX for direct
    return NdotX / max(NdotX * (1.0 - k) + k, 1e-6);
}

float smith_g(float NdotV, float NdotL, float a)
{
    return smith_g1(NdotV, a) * smith_g1(NdotL, a);
}

vec3 fresnel_schlick(float cos_theta, vec3 F0, float F90)
{
    return F0 + (vec3(F90) - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// KHR_materials_specular, with the base colour, metalness and the material's specular colour
// and strength (MaterialEval::specular_color_factor, specular_factor): reflectance at normal
// incidence, and in `F90` at grazing. A dielectric's is the strength times 0.04 tinted by the
// colour (capped at 1), and the strength; a metal's its base colour, and 1.
vec3 velk_specular_f0(vec3 base, float metallic, vec3 specular_color, float specular, out float F90)
{
    specular = clamp(specular, 0.0, 1.0);
    F90 = mix(specular, 1.0, metallic);
    return mix(min(0.04 * max(specular_color, vec3(0.0)), vec3(1.0)) * specular, base, metallic);
}

// The way from `pos` to `light`, `L`, and how much of the light reaches there, `atten`: all of a
// directional light's; a point or spot light's by its range, a spot's by its cone too. A square
// area light (type 3) counts here as a point at its middle; the deferred lighting and the path
// tracer treat those their own way and do not call this for them.
void velk_light_delta(Light light, vec3 pos, out vec3 L, out float atten)
{
    atten = 1.0;
    if (light.flags.x == 0u) {
        L = -light.direction.xyz;
    } else {
        vec3 to_light = light.position.xyz - pos;
        float dist = length(to_light);
        L = to_light / max(dist, 1e-6);
        float range = max(light.params.x, 1e-6);
        float t = clamp(1.0 - dist / range, 0.0, 1.0);
        atten = t * t;
        if (light.flags.x == 2u) {
            float cos_a = dot(-L, light.direction.xyz);
            atten *= smoothstep(light.params.z, light.params.y, cos_a);
        }
    }
}
