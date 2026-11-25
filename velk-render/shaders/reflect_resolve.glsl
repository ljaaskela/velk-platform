#define VELK_REFLECT_CUTOFF 0.3
#define VELK_REFLECT_SHARP 0.05

// The last frame's result where the place `at` (relative to the camera, or with `w` 0 a direction)
// was on screen; no frames off screen.
vec4 velk_reflect_last(vec3 at, float w, uint width, uint height)
{
    GlobalData g = VELK_GLOBALS;
    vec4 clip = g.prev_view_projection * vec4(w > 0.5 ? at + g.cam_moved : at, w);
    if (clip.w <= 1e-5) return vec4(0.0);
    vec2 p = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(width), float(height));
    if (any(lessThan(p, vec2(0.0))) || p.x >= float(width) || p.y >= float(height)) return vec4(0.0);
    return texelFetch(velk_textures[nonuniformEXT(pc.more.y)], ivec2(p), 0);
}

void main()
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(width) || coord.y >= int(height)) return;
    vec4 mat = texelFetch(velk_textures[nonuniformEXT(pc.more.x)], coord, 0);
    float roughness = clamp(mat.g, 0.04, 1.0);
    vec3 n4 = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], coord, 0).xyz;
    if (uint(mat.b * 255.0 + 0.5) == 0u || dot(n4, n4) < 1e-6 || roughness >= VELK_REFLECT_CUTOFF) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, vec4(0.0));
        return;
    }
    vec3 n = normalize(n4);
    vec3 pos = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], coord, 0).xyz;
    ivec2 last = ivec2(int(width) - 1, int(height) - 1);
    ivec2 blocks_last = ivec2(int((width + 1u) / 2u) - 1, int((height + 1u) / 2u) - 1);
    uint k = VELK_GLOBALS.present_counter & 3u;
    ivec2 phase = ivec2(int(k & 1u), int(k >> 1u));
    ivec2 own_block = coord / 2;
    bool sharp = roughness < VELK_REFLECT_SHARP;
    // This frame's samples: the own block's for a mirror, the 3 by 3 blocks' for a glossy surface.
    int reach = sharp ? 0 : 1;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    vec3 lo = vec3(1e30);
    vec3 hi = vec3(-1e30);
    vec4 own = vec4(0.0);
    float t_any = 0.0;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            ivec2 b = clamp(own_block + ivec2(dx, dy), ivec2(0), blocks_last);
            vec4 s = texelFetch(velk_textures[nonuniformEXT(pc.extras.x)], b, 0);
            if (s.a == 0.0) continue;
            ivec2 q = min(b * 2 + phase, last);
            vec3 qn = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], q, 0).xyz;
            if (dot(qn, qn) < 1e-6) continue;
            vec3 qp = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], q, 0).xyz;
            float w = pow(max(dot(normalize(qn), n), 0.0), 8.0) * exp(-abs(dot(n, qp - pos)) / (0.02 * length(pos) + 1e-3));
            if (w <= 1e-4) continue;
            sum += w * s.rgb;
            total += w;
            lo = min(lo, s.rgb);
            hi = max(hi, s.rgb);
            if (dx == 0 && dy == 0) own = s;
            if (t_any == 0.0) t_any = s.a;
        }
    }
    // The last result, through the reflection's image, else where the surface was.
    float t = own.a != 0.0 ? own.a : t_any;
    vec4 was = vec4(0.0);
    if (pc.more.w == 0u) {
        vec3 view = normalize(pos);
        if (t != 0.0) was = t > 0.0 ? velk_reflect_last(pos + view * t, 1.0, width, height) : velk_reflect_last(view, 0.0, width, height);
        if (was.a <= 0.0) was = velk_reflect_last(pos, 1.0, width, height);
    }
    if (sharp) {
        bool traced = coord == min(own_block * 2 + phase, last);
        vec3 result = vec3(0.0);
        float frames = 1.0;
        if (traced && own.a != 0.0) result = own.rgb;
        else if (was.a > 0.0) result = was.rgb;
        else if (total > 0.0) result = sum / total;
        else frames = 0.0;
        imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, vec4(result, frames));
        return;
    }
    if (total <= 0.0) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, was.a > 0.0 ? was : vec4(0.0));
        return;
    }
    vec3 now = sum / total;
    vec3 history = now;
    float frames = 0.0;
    if (was.a > 0.0) {
        vec3 spread = (hi - lo) * 0.25 + vec3(0.02);
        history = clamp(was.rgb, lo - spread, hi + spread);
        frames = was.a;
    }
    frames = min(frames + 1.0, float(pc.env.w));
    imageStore(gStorageImagesF16[nonuniformEXT(pc.more.z)], coord, vec4(mix(history, now, 1.0 / frames), frames));
}
