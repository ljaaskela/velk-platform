#version 450
#define VELK_COMPUTE 1
#include "velk.glsl"
#include "velk-ui.glsl"

// A work group of VELK_RT_GROUP_THREADS threads in a row where the composer defines it
// (compose_rt_trace), else 8 by 8.
#ifdef VELK_RT_GROUP_THREADS
layout(local_size_x = VELK_RT_GROUP_THREADS, local_size_y = 1, local_size_z = 1) in;
#else
#define VELK_RT_GROUP_THREADS 64
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

layout(set = 0, binding = 1, rgba8) uniform writeonly image2D gStorageImages[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
// Float storage, for targets carrying HDR rather than display values.
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];

// The per-view frame state (inv-VP, cam, the view's top tree for rays, present_counter)
// comes from the bound FrameGlobals record, indexed by pc.globals_base. See VELK_GLOBALS
// below.

// Scene light. Mirrors the C++ GpuLight struct (80 bytes).
struct Light {
    uvec4 flags;           // x = type (0=dir, 1=point, 2=spot), y = shadow_tech_id, zw = _
    vec4  position;        // xyz = world position (point / spot)
    vec4  direction;       // xyz = world forward axis (directional / spot)
    vec4  color_intensity; // rgb = colour, a = intensity multiplier
    vec4  params;          // x = range, y = cos(inner), z = cos(outer), w = light size (dir: angular radius rad; point/spot: world radius)
};

// The place on a square area light that velk_light_sample chose last, which the shadow ray toward
// that light aims at, so that its light and what blocks it are of the same place.
vec3 velk_light_target = vec3(0.0);

// Scene lights bound by index (set = 1 slot 5); this view's run starts at
// pc.lights_base. Same buffer the deferred-lighting compute reads.
layout(set = 1, binding = 5, std430) readonly buffer VelkLights { Light data[]; } velk_lights;

// Per-primitive ray geometry (set = 1 slot 11), plus the trees it points at (slots 12 / 13),
// the same buffers the deferred prelude reads. A primitive's runs are allocated once at load
// and never move.
layout(set = 1, binding = 11, std430) readonly buffer VelkMeshStatic { MeshStaticData data[]; } velk_mesh_static_records;
layout(set = 1, binding = 12, std430) readonly buffer VelkMeshNodes { VelkMeshNode data[]; } velk_mesh_nodes;
layout(set = 1, binding = 13, std430) readonly buffer VelkClusterWords { uint data[]; } velk_cluster_words;

// Material records as raw words (set = 1 slot 4). The raster path binds this
// same slot as a typed block, which one pipeline can do because it compiles
// for a single material type; this shader composes many, so it reads the
// bytes and each material's generated velk_unpack_<T> rebuilds its own struct
// from them. Materials whose layout cannot be derived stay on device
// addresses and never touch this buffer.
layout(set = 1, binding = 4, std430) readonly buffer VelkMaterialWords { uint data[]; } velk_material_words;

// This shader composes many materials, so slot 4 is bound above as raw words
// and must not be re-declared as a typed block: VELK_MATERIAL(T) becomes a
// no-op, and the composer replaces each material's VELK_MATERIAL(T) line with
// its generated velk_unpack_<T>. The load pastes the type name onto that
// function, so every snippet's `VELK_LOAD_MATERIAL(T, ctx)` resolves to its
// own reader. A material whose record cannot be modelled gets no reader and
// fails to compile here, rather than silently reading the wrong bytes.
#undef VELK_MATERIAL
#undef VELK_LOAD_MATERIAL
#define VELK_MATERIAL(T)
#define VELK_LOAD_MATERIAL(T, ctx) velk_unpack_##T((ctx).material_base)

// RT root: per-dispatch state pushed inline as a push constant (no device
// address anywhere). scalar layout matches the C++ `RtRoot` struct.
//
// The camera matrices, BVH root/counts/bases and present_counter live in
// the bound FrameGlobals record (indexed by globals_base), not here. Only
// the RT-specific per-dispatch state remains, all indices / inline data.
// Baked into the cached secondary at record time like the deferred PC;
// globals_base rotates with the globals ring but the cached pass re-records
// on view change, and static globals match across ring slots.
layout(push_constant, scalar) uniform PC {
    uint shapes_base;           // the main's own: the reference path's restart, unused otherwise
    uint globals_base;          // FrameGlobals index (set = 1 slot 2)
    uint light_count;
    uint lights_base;           // index into velk_lights (set = 1 slot 5)
    vec2 env_params;            // x = intensity, y = rotation_rad (inline)
    uvec4 extras;               // the main's own: for the RT path x=image_index, y=width, z=height
    uvec4 env;                  // x=env_material_id, y=env_texture_id, zw=_
    uvec4 more;                 // the main's own, past what extras holds; unset where unused
} pc;
// This view's FrameGlobals, read by index from the bound globals buffer
// (set = 1 slot 2, declared in velk.glsl) instead of a device address.
#define VELK_GLOBALS velk_globals.data[pc.globals_base]

// A scene light, with its position relative to the camera.
Light velk_light_at(uint index)
{
    Light light = velk_lights.data[index];
    light.position.xyz -= VELK_GLOBALS.cam_pos.xyz;
    return light;
}

// ===== Core ray / hit / fill-context types =====
#include "velk_intersect.glsl"

// EvalContext + MaterialEval shared between raster and RT come from
// velk-ui.glsl (included above).

// Result of evaluating a material at a hit. GLSL forbids recursion, so
// materials cannot call trace_ray; instead they return the local emission
// they produce at this hit plus (optionally) a bounced ray for the
// iterative trace loop in main() to continue with.
//
//   emission    rgb = color contributed at this hit (already weighted for
//               alpha at primary); a = opacity for primary compositing.
//   throughput  multiplier applied to the next bounce's contribution.
//   next_dir    world-space direction of the next ray (ignored if
//               terminate == true).
//   terminate   true = no further bounces (flat UI material, text, env).
//   sample_count_hint  material's preferred number of bounce samples at
//               this hit (e.g. 1 for a mirror, higher for rough GGX).
//               Tracer caps against a global per-bounce budget.
struct BrdfSample {
    vec4 emission;
    vec3 throughput;
    vec3 next_dir;
    bool terminate;
    uint sample_count_hint;
};

// The walk's stack per thread, in shared memory: 48 for 64 threads is 12 KB, for 32 threads 6 KB,
// as the light trace's, since partitions' trees may be 16 levels deeper than balanced. The light
// trace's log gives the deepest the probes' rays got.
#define VELK_TRACE_STACK_SIZE 48
#include "velk_trace.glsl"


// ===== RNG: PCG-hash seeded by (pixel, frame). =====
uint g_rng_state;

uint pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

void rng_init(uvec2 coord, uint frame) {
    g_rng_state = pcg_hash(coord.x ^ pcg_hash(coord.y ^ pcg_hash(frame + 1u)));
}

uint rng_next_uint() {
    g_rng_state = pcg_hash(g_rng_state);
    return g_rng_state;
}

float rng_next_float() {
    // 24-bit mantissa precision; top 8 bits unused.
    return float(rng_next_uint() >> 8u) * (1.0 / 16777216.0);
}

vec2 rng_next_vec2() { return vec2(rng_next_float(), rng_next_float()); }

// ===== GGX microfacet sampling (isotropic). =====
// Returns a unit half-vector H in world space distributed as GGX(roughness).
vec3 ggx_sample_half(vec3 N, float roughness, vec2 xi) {
    float a = roughness * roughness;
    float phi = 2.0 * 3.14159265 * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

    vec3 H_local = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    // Build a tangent basis around N. Uses a standard "pick non-parallel up"
    // trick to avoid degenerate cross products.
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * H_local.x + B * H_local.y + N * H_local.z);
}

// Sample a reflection direction: pick a microfacet H, reflect V around it.
vec3 ggx_sample_reflect(vec3 V, vec3 N, float roughness, vec2 xi) {
    vec3 H = ggx_sample_half(N, roughness, xi);
    return reflect(-V, H);
}

// Forward declaration. The composer emits the actual definition after
// material #includes. Materials are pure (no recursion into trace_ray).
BrdfSample velk_resolve_fill(uint mid, EvalContext ctx);

// Forward declaration for the shadow-technique dispatch. Emitted by
// the composer after shadow-technique #includes. Materials that want
// to attenuate their direct-lighting contribution by occlusion call
// this with the light's shadow_tech_id; tech_id 0 returns 1.0 (fully
// lit, no shadow technique attached to that light).
float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal);

// Sample the environment along a direction (or return black if the
// camera has no env). Inlined here rather than going through the env
// material's velk_fill_env, because materials may need to call this
// (e.g. StandardMaterial's diffuse term) and calling velk_resolve_fill
// from inside a fill would re-introduce the recursion GLSL forbids.
// Mirrors EnvMaterial's equirect sampling; env params (x = intensity,
// y = rotation_rad) ride the push constant inline (pc.env_params).
vec3 env_miss_color(vec3 rd) {
    if (pc.env.x == 0u) return vec3(0.0);
    const float PI = 3.14159265358979323846;
    float c = cos(pc.env_params.y);
    float s = sin(pc.env_params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5;
    return velk_texture(pc.env.y, vec2(u, v)).rgb * pc.env_params.x;
}
