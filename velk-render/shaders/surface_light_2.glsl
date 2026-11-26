// Roughness-aware Schlick for environment / IBL specular: scales the grazing
// reflectance (F90) by (1 - roughness), so rough surfaces don't reflect the
// full environment at glancing angles. Without this, rough geometry edges
// (e.g. painted facades) get an over-bright Fresnel rim of the env. A weaker
// specular strength (F90 below 1) weakens the whole lobe in proportion.
vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float F90, float roughness)
{
    vec3 grazing = max(vec3((1.0 - roughness) * F90), F0);
    return F0 + (grazing - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}
