// Mirrors C++ GpuLight (80 bytes) in ray_tracer.cpp / deferred_lighter.cpp.
struct Light {
    uvec4 flags;           // x = type (0 dir, 1 point, 2 spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz world position (point / spot)
    vec4  direction;       // xyz world forward (dir / spot)
    vec4  color_intensity; // rgb colour, a intensity
    vec4  params;          // x range, y cos(inner), z cos(outer), w light size (dir: angular radius rad; point/spot: world radius)
};

// The place on a square area light that velk_light_sample chose last, which the shadow ray toward
// that light aims at, so that its light and what blocks it are of the same place.
vec3 velk_light_target = vec3(0.0);

// Scene lights bound by index (set = 1 slot 5); this view's run starts at
// pc.lights_base. Same buffer the RT compute reads.
layout(set = 1, binding = 5, std430) readonly buffer VelkLights { Light data[]; } velk_lights;

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

// Equirect env sample with explicit mip LOD. The env texture ships
// with a bilinear-downsampled mip chain as a rough roughness
// prefilter; higher `lod` values read blurrier mips. Returns vec3(0)
// when the view has no environment.
vec3 env_miss_color_lod(vec3 rd, float lod)
{
    if (pc.env_texture_id == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    vec2 params = pc.env_params;
    float c = cos(params.y);
    float s = sin(params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return textureLod(velk_textures[nonuniformEXT(pc.env_texture_id)],
                      vec2(u, v), lod).rgb * params.x;
}

vec3 env_miss_color(vec3 rd) { return env_miss_color_lod(rd, 0.0); }
#velk_include VELK_LIGHT_EVAL_GLSL
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
