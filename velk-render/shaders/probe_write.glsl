uint velk_probe_oct_encode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return packSnorm2x16(e);
}

// Halves past their range would be infinite: kept within it.
uint velk_probe_halves(float a, float b)
{
    return packHalf2x16(clamp(vec2(a, b), vec2(-65000.0), vec2(65000.0)));
}

// Writes the light `p` at `at`, six words as VELK_PROBE_LIGHT holds them.
void velk_probe_write_light(uint at, VelkProbeLight p)
{
    VELK_LIGHT_WORDS.data[at] = velk_probe_halves(p.e.x, p.e.y);
    VELK_LIGHT_WORDS.data[at + 1u] = velk_probe_halves(p.e.z, p.r.x);
    VELK_LIGHT_WORDS.data[at + 2u] = velk_probe_halves(p.r.y, p.r.z);
    VELK_LIGHT_WORDS.data[at + 3u] = velk_probe_halves(p.g.x, p.g.y);
    VELK_LIGHT_WORDS.data[at + 4u] = velk_probe_halves(p.g.z, p.b.x);
    VELK_LIGHT_WORDS.data[at + 5u] = velk_probe_halves(p.b.y, p.b.z);
}

// Writes probe `at` whole: its light, where it is, its normal and its frames.
void velk_probe_write(uint at, VelkProbeLight p, vec3 pos, vec3 n, float frames)
{
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT] = velk_probe_halves(p.e.x, p.e.y);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 1u] = velk_probe_halves(p.e.z, p.r.x);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 2u] = velk_probe_halves(p.r.y, p.r.z);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 3u] = velk_probe_halves(p.g.x, p.g.y);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 4u] = velk_probe_halves(p.g.z, p.b.x);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_LIGHT + 5u] = velk_probe_halves(p.b.y, p.b.z);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION] = floatBitsToUint(pos.x);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 1u] = floatBitsToUint(pos.y);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_POSITION + 2u] = floatBitsToUint(pos.z);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_NORMAL] = velk_probe_oct_encode(n);
    VELK_LIGHT_WORDS.data[at + VELK_PROBE_FRAMES] = floatBitsToUint(frames);
}
