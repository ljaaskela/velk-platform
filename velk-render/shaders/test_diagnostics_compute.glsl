#version 450
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 2, rgba32f) uniform writeonly image2D gStorageImagesF32[];
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];
layout(set = 1, binding = 16, std430) buffer TestWords { uint data[]; } test_words;

layout(push_constant, scalar) uniform PC {
    uint word;
    uint words;
    uint mode;
    uint globals_base;
    uint normal_id;
    uint pos_id;
    uint albedo_id;
    uint color_id;
    uint history_id;
    uint output_id;
    uint width;
    uint height;
    uint tiles_up_only;
    uint view_id;
} pc;

const uint kTilesX = 16u;
const uint kTilesY = 9u;
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

void add(uint at, uint amount) { atomicAdd(test_words.data[pc.word + at], amount); }

void main()
{
    if (pc.mode == 0u) {
        uint i = gl_LocalInvocationIndex + gl_WorkGroupID.x * 64u;
        if (i < pc.words) test_words.data[pc.word + i] = 0u;
        return;
    }
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.width) || p.y >= int(pc.height)) return;
    if (pc.view_id != 0u) imageStore(gStorageImagesF16[nonuniformEXT(pc.view_id)], p, vec4(0.0, 0.0, 0.0, 1.0));

    vec3 normal = texelFetch(velk_textures[nonuniformEXT(pc.normal_id)], p, 0).xyz;
    if (dot(normal, normal) < 1e-6) {
        imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(0.0));
        return;
    }
    vec3 N = normalize(normal);
    vec3 rel = texelFetch(velk_textures[nonuniformEXT(pc.pos_id)], p, 0).xyz;
    vec3 albedo = texelFetch(velk_textures[nonuniformEXT(pc.albedo_id)], p, 0).rgb;
    vec3 color = texelFetch(velk_textures[nonuniformEXT(pc.color_id)], p, 0).rgb;
    float light = max(dot(color, kLuma) / max(dot(albedo, kLuma), 0.04), 1e-4);
    GlobalData g = velk_globals.data[pc.globals_base];
    if (pc.mode == 5u) {
        light = max(dot(albedo, kLuma), 1e-4);
    }
    imageStore(gStorageImagesF32[nonuniformEXT(pc.output_id)], p, vec4(rel, light));

    vec3 prev_pos = rel + g.cam_moved;
    vec4 prev_clip = g.prev_view_projection * vec4(prev_pos, 1.0);
    if (prev_clip.w <= 1e-6) return;
    vec2 prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThanEqual(prev_uv, vec2(1.0)))) return;
    ivec2 q = ivec2(prev_uv * vec2(float(pc.width), float(pc.height)));
    ivec2 last = ivec2(int(pc.width) - 1, int(pc.height) - 1);

    // The range of the light on this surface around where it was. Reprojection lands between
    // pixels, so only light outside what was nearby counts as having changed.
    mat4 vp = g.view_projection;
    float focal = length(vec3(vp[0][1], vp[1][1], vp[2][1]));
    float pixel = length(rel) * 2.0 / (focal * float(pc.height));
    float lowest = 1e30;
    float highest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 history = texelFetch(velk_textures[nonuniformEXT(pc.history_id)], clamp(q + ivec2(x, y), ivec2(0), last), 0);
            if (history.a > 0.0 && length(history.xyz - prev_pos) <= pixel * 6.0) {
                lowest = min(lowest, history.a);
                highest = max(highest, history.a);
            }
        }
    }
    if (highest <= 0.0) return;

    float nearest = clamp(light, lowest, highest);
    float change = abs(light - nearest) / max(max(light, nearest), 1e-3);
    // White where the surface faces up or down, blue elsewhere.
    if (pc.view_id != 0u) {
        float shown = min(change * 8.0, 1.0);
        float up = abs(N.y) > 0.9 ? shown : 0.0;
        imageStore(gStorageImagesF16[nonuniformEXT(pc.view_id)], p, vec4(up, up, shown, 1.0));
    }
    uint scaled = uint(change * 256.0);
    add(0u, 1u);
    add(1u, scaled);
    if (change > 0.05) add(2u, 1u);
    if (change > 0.2) add(3u, 1u);
    if (abs(N.y) > 0.9) {
        add(4u, 1u);
        add(5u, scaled);
        if (change > 0.05) add(6u, 1u);
        if (change > 0.2) add(7u, 1u);
    }
    if (pc.tiles_up_only != 0u && abs(N.y) <= 0.9) return;
    uint tile = min(uint(p.y) * kTilesY / pc.height, kTilesY - 1u) * kTilesX + min(uint(p.x) * kTilesX / pc.width, kTilesX - 1u);
    add(10u + tile, 1u);
    add(10u + kTilesX * kTilesY + tile, scaled);
}
