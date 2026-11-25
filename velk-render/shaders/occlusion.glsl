bool velk_behind_pyramid(uint pyramid, mat4 vp, vec3 center, float radius)
{
    uint levels = cull_words.data[pyramid];
    if (levels == 0u) return false;

    // The sphere's rectangle on screen and its nearest depth, from the sphere itself (Mara and
    // McGuire 2013): vp's rows are the screen's axes, scaled by the projection.
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    vec2 lo;
    vec2 hi;
    float nearest;
    float lw = length(r3);
    if (lw < 1e-12) {
        // Orthographic: the centre moved by the radius along each axis.
        float w = vp[3][3];
        if (w <= 1e-5) return false;
        vec2 at = vec2(dot(r0, center) + vp[3][0], dot(r1, center) + vp[3][1]);
        vec2 reach = radius * vec2(length(r0), length(r1));
        lo = (at - reach) / w;
        hi = (at + reach) / w;
        nearest = (dot(r2, center) + vp[3][2] + radius * length(r2)) / w;
    } else {
        // In the camera's axes, z its distance ahead; the tangent lines to the sphere bound it.
        float px = length(r0);
        float py = length(r1);
        vec3 c = vec3((dot(r0, center) + vp[3][0]) / px, (dot(r1, center) + vp[3][1]) / py, (dot(r3, center) + vp[3][3]) / lw);
        if (c.z - radius <= 1e-5) return false;
        vec3 cr = c * radius;
        float czr2 = c.z * c.z - radius * radius;
        float vx = sqrt(c.x * c.x + czr2);
        float vy = sqrt(c.y * c.y + czr2);
        vec2 scale = vec2(px, py) / lw;
        lo = vec2((vx * c.x - cr.z) / (vx * c.z + cr.x), (vy * c.y - cr.z) / (vy * c.z + cr.y)) * scale;
        hi = vec2((vx * c.x + cr.z) / (vx * c.z - cr.x), (vy * c.y + cr.z) / (vy * c.z - cr.y)) * scale;
        // Its point nearest the camera.
        vec4 clip = vp * vec4(center - r3 / lw * radius, 1.0);
        nearest = clip.z / clip.w;
    }

    uvec2 first_size = uvec2(cull_words.data[pyramid + 1u], cull_words.data[pyramid + 2u]);
    vec2 a = clamp(lo * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * vec2(first_size);
    vec2 b = clamp(hi * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * vec2(first_size);

    // The level at which the rectangle spans at most two texels each way.
    float extent = max(max(b.x - a.x, b.y - a.y), 1.0);
    uint level = min(uint(ceil(log2(extent))), levels - 1u);
    uvec2 size = max(first_size >> level, uvec2(1u));
    uvec2 from = min(uvec2(a) >> level, size - 1u);
    uvec2 to = min(uvec2(b) >> level, size - 1u);
    uint words = cull_words.data[pyramid + 3u + level];
    float farthest = 1e30;
    for (uint y = from.y; y <= to.y; ++y) {
        for (uint x = from.x; x <= to.x; ++x) {
            farthest = min(farthest, uintBitsToFloat(cull_words.data[words + y * size.x + x]));
        }
    }
    return nearest < farthest;
}
