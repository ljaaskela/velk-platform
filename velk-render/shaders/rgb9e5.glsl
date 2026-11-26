
// An RGB9E5 colour (rgb9e5 in gpu_data.h).
vec3 velk_rgb9e5(uint w)
{
    return vec3(float(w & 511u), float((w >> 9u) & 511u), float((w >> 18u) & 511u)) * exp2(float(int(w >> 27u) - 24));
}

// `c` as RGB9E5, as rgb9e5 in gpu_data.h packs it, each colour below 65000 so that it never
// packs to VELK_LIGHT_NONE.
uint velk_pack_rgb9e5(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(65000.0));
    float most = max(max(c.r, c.g), c.b);
    int e = most > 0.0 ? clamp(int(floor(log2(most))) + 1, -15, 16) : 0;
    float scale = exp2(float(9 - e));
    // Rounding can carry the largest mantissa to 512: one exponent more.
    if (uint(most * scale + 0.5) >= 512u && e < 16) {
        ++e;
        scale *= 0.5;
    }
    uvec3 m = min(uvec3(c * scale + 0.5), uvec3(511u));
    return m.r | (m.g << 9u) | (m.b << 18u) | (uint(e + 15) << 27u);
}
