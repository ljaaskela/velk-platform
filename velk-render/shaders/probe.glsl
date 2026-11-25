// A probe's light: `e` the mean radiance arriving at its own normal, weighted by cosine, and for
// each colour the L1 direction of the radiance, three times its mean times the direction.
struct VelkProbeLight {
    vec3 e;
    vec3 r;
    vec3 g;
    vec3 b;
};

VelkProbeLight velk_probe_read(uint at)
{
    vec2 h0 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT]);
    vec2 h1 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 1u]);
    vec2 h2 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 2u]);
    vec2 h3 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 3u]);
    vec2 h4 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 4u]);
    vec2 h5 = unpackHalf2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 5u]);
    VelkProbeLight p;
    p.e = vec3(h0.x, h0.y, h1.x);
    p.r = vec3(h1.y, h2.x, h2.y);
    p.g = vec3(h3.x, h3.y, h4.x);
    p.b = vec3(h4.y, h5.x, h5.y);
    return p;
}

// The probe's normal at `at`, octahedral.
vec3 velk_probe_normal(uint at)
{
    vec2 e = unpackSnorm2x16(VELK_LIGHT_WORDS.data[at + VELK_PROBE_NORMAL]);
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

vec3 velk_probe_position(uint at)
{
    return vec3(uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION]),
                uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 1u]),
                uintBitsToFloat(VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 2u]));
}

// What a probe at normal `pn` with light `p` brings a surface facing `n`, unclamped: its light at
// its own normal, moved by its directions to `n`. For sums, clamped once after them.
vec3 velk_probe_eval_raw(VelkProbeLight p, vec3 pn, vec3 n)
{
    vec3 d = n - pn;
    return p.e + vec3(dot(p.r, d), dot(p.g, d), dot(p.b, d)) / 3.0;
}

// The same, never below zero.
vec3 velk_probe_eval(VelkProbeLight p, vec3 pn, vec3 n)
{
    return max(velk_probe_eval_raw(p, pn, n), vec3(0.0));
}
