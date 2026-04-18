#include "standard_material.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::impl {

namespace {

VELK_GPU_STRUCT StandardParams
{
    ::velk::color base_color;
    float metallic;
    float roughness;
    float _pad[2];
};
static_assert(sizeof(StandardParams) == 32,
              "StandardParams must be 32 bytes (std430 + alignas(16))");

// Raster: a plain base-colour fill. Proper PBR lives in the RT path only.
// Vertex shader emits v_color so we reuse the default fragment.
constexpr string_view standard_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer StandardParams {
    vec4 base_color;
    float metallic;
    float roughness;
    float _pad0;
    float _pad1;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    StandardParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    gl_Position = root.global_data.view_projection * inst.world_matrix * local_pos;
    v_color = root.material.base_color;
    v_local_uv = q;
    v_size = inst.size;
}
)";

constexpr string_view standard_deferred_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer StandardParams {
    vec4 base_color;
    float metallic;
    float roughness;
    float _pad0;
    float _pad1;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    StandardParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_world_normal;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    vec4 world_pos_h = inst.world_matrix * local_pos;
    gl_Position = root.global_data.view_projection * world_pos_h;
    v_world_pos = world_pos_h.xyz;
    v_world_normal = normalize(vec3(inst.world_matrix[2]));
}
)";

// Deferred fragment: writes all four G-buffer attachments. Reads the
// per-draw material values (base_color, metallic, roughness) straight
// from the DrawData buffer, matching the convention the RT path uses.
constexpr string_view standard_deferred_fragment_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer StandardParams {
    vec4 base_color;
    float metallic;
    float roughness;
    float _pad0;
    float _pad1;
};

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    StandardParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_world_normal;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec4 g_normal;
layout(location = 2) out vec4 g_world_pos;
layout(location = 3) out vec4 g_material;

void main()
{
    g_albedo    = root.material.base_color;
    g_normal    = vec4(normalize(v_world_normal), 0.0);
    g_world_pos = vec4(v_world_pos, 0.0);
    g_material  = vec4(root.material.metallic, root.material.roughness, 1.0 / 255.0 /*Standard*/, 0.0);
}
)";

// RT fill snippet. Produces the local shading (diffuse via env at N) and
// hands the specular bounce back to the main loop as next_dir+throughput.
// GLSL's no-recursion rule means we can't call trace_ray from here; the
// iterative loop in main() is what actually evaluates the bounce.
constexpr string_view standard_fill_src = R"(
layout(buffer_reference, std430) readonly buffer StandardMaterialData {
    vec4 base_color;
    vec4 params;  // x = metallic, y = roughness, zw unused
};

BrdfSample velk_fill_standard(FillContext ctx)
{
    StandardMaterialData d = StandardMaterialData(ctx.data_addr);

    vec3 N = normalize(ctx.normal);
    vec3 V = normalize(-ctx.ray_dir);
    float metallic  = clamp(d.params.x, 0.0, 1.0);
    float roughness = clamp(d.params.y, 0.04, 1.0);

    // Fresnel at normal incidence: 0.04 for dielectrics, base_color for metals.
    vec3 F0 = mix(vec3(0.04), d.base_color.rgb, metallic);
    float VdotN = max(dot(V, N), 0.0);
    vec3 F = F0 + (vec3(1.0) - F0) * pow(1.0 - VdotN, 5.0);

    // Direct lighting from scene lights. Each light contributes a
    // Lambertian diffuse term scaled by its distance / spot attenuation
    // and modulated by its shadow technique's visibility. Specular
    // highlight for direct lights is left to the bounce path for now;
    // once that shows up as a real limitation we can add analytic
    // GGX specular here too.
    vec3 direct = vec3(0.0);
    for (uint li = 0u; li < pc.light_count; ++li) {
        Light light = pc.lights.data[li];
        vec3 L;
        float atten = 1.0;
        if (light.flags.x == 0u) {
            // Directional light: L = -forward axis, no distance falloff.
            L = -light.direction.xyz;
        } else {
            vec3 to_light = light.position.xyz - ctx.hit_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 1e-6);
            float range = max(light.params.x, 1e-6);
            float t = clamp(1.0 - dist / range, 0.0, 1.0);
            atten = t * t;
            if (light.flags.x == 2u) {
                // Spot: smooth cosine falloff between inner and outer.
                float cos_a = dot(-L, light.direction.xyz);
                atten *= smoothstep(light.params.z, light.params.y, cos_a);
            }
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) continue;
        float shadow = velk_eval_shadow(light.flags.y, li, ctx.hit_pos, N);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.a * atten * shadow;
        direct += d.base_color.rgb * (1.0 - metallic) * NdotL * radiance;
    }

    // Diffuse term: crude "irradiance at the normal" via a single env sample.
    // TODO: proper diffuse lighting is a cosine-weighted integral of the env
    // over the upper hemisphere at N. A single point sample underestimates
    // the integral and reads as "too sharp" (harder light/shadow transitions
    // than a real PBR pipeline). Two standard fixes, either of which we can
    // add later:
    //   (a) Preconvolve the env once into an irradiance cubemap at init time
    //       and sample that here. Deterministic, one extra texture.
    //   (b) Stochastically sample N cosine-weighted directions and average.
    //       Adds noise, needs accumulation to converge.
    // For UI-scale scenes with HDRI skyboxes the single-sample approximation
    // is visually adequate; revisit when it becomes objectionable.
    vec3 env_at_normal = env_miss_color(N);
    vec3 diffuse = d.base_color.rgb * (1.0 - metallic) * env_at_normal;
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // Specular: sample a GGX half-vector, reflect around it. Main's loop
    // will evaluate the reflected ray and apply F as throughput.
    vec3 H = ggx_sample_half(N, roughness, rng_next_vec2());
    vec3 L = reflect(-V, H);

    BrdfSample bs;
    bs.emission = vec4(kD * diffuse + direct, d.base_color.a);
    bs.throughput = F;
    bs.next_dir = L;
    bs.terminate = false;
    return bs;
}
)";

} // namespace

uint64_t StandardMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    // Reuses the registered default fragment shader (solid v_color passthrough).
    return ensure_pipeline(ctx, /*fragment*/ {}, standard_vertex_src);
}

size_t StandardMaterial::get_draw_data_size() const
{
    return sizeof(StandardParams);
}

ReturnValue StandardMaterial::write_draw_data(void* out, size_t size) const
{
    if (auto state = read_state<IStandard>(this)) {
        return set_material<StandardParams>(out, size, [&](auto& p) {
            p.base_color = state->base_color;
            p.metallic   = state->metallic;
            p.roughness  = state->roughness;
        });
    }
    return ReturnValue::Fail;
}

string_view StandardMaterial::get_snippet_fn_name() const { return "velk_fill_standard"; }
string_view StandardMaterial::get_snippet_source() const  { return standard_fill_src; }

ShaderSource StandardMaterial::get_raster_source(IRasterShader::Target t) const
{
    switch (t) {
    case IRasterShader::Target::Deferred:
        return {standard_deferred_vertex_src, standard_deferred_fragment_src};
    case IRasterShader::Target::Forward:
    default:
        // Forward pipeline is already compiled by get_pipeline_handle
        // (default fragment + standard_vertex_src); no override needed.
        return {};
    }
}

} // namespace velk::impl
