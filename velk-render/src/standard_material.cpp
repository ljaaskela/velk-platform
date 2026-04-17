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

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    vec4 base_color;
    float metallic;
    float roughness;
    float _pad0;
    float _pad1;
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
    v_color = root.base_color;
    v_local_uv = q;
    v_size = inst.size;
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
    bs.emission = vec4(kD * diffuse, d.base_color.a);
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

size_t StandardMaterial::gpu_data_size() const
{
    return sizeof(StandardParams);
}

ReturnValue StandardMaterial::write_gpu_data(void* out, size_t size) const
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

string_view StandardMaterial::get_fill_src() const          { return standard_fill_src; }
string_view StandardMaterial::get_fill_fn_name() const      { return "velk_fill_standard"; }
string_view StandardMaterial::get_fill_include_name() const { return "velk_standard.glsl"; }

} // namespace velk::impl
