// The filter: a thread a probe, from the ray store into this frame's.
void velk_probes_filter()
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint down = (height + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint probe = gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex;
    if (probe >= across * down) return;
    // The probe list at pc.more.w empty again for the next frame's gather: its count and its
    // trace's groups.
    if (probe == 0u && pc.more.w != 0xFFFFFFFFu) {
        probe_words.data[pc.more.w] = 0u;
        probe_words.data[pc.more.w + 1u] = 0u;
    }
    uint raw_store = pc.extras.x;
    uint mine = raw_store + probe * VELK_PROBE_WORDS;
    uint now = pc.extras.z + probe * VELK_PROBE_WORDS;
    float frames = uintBitsToFloat(probe_words.data[mine + VELK_PROBE_FRAMES]);
    if (frames <= 0.0) {
        for (uint w = 0u; w < VELK_PROBE_WORDS; ++w) probe_words.data[now + w] = 0u;
        return;
    }
    vec3 pos = velk_probe_position(mine);
    vec3 n = velk_probe_normal(mine);
    float near = 0.02 * length(pos) + 1e-3;

    // Its own and its 3 by 3 neighbours' light, each by how near the two surfaces are to each
    // other's planes and how much their normals agree.
    ivec2 cell = ivec2(int(probe % across), int(probe / across));
    VelkProbeLight light;
    light.e = vec3(0.0);
    light.r = vec3(0.0);
    light.g = vec3(0.0);
    light.b = vec3(0.0);
    float total = 0.0;
    for (int k = 0; k < 9; ++k) {
        ivec2 c = cell + ivec2(k % 3 - 1, k / 3 - 1);
        if (c.x < 0 || c.y < 0 || c.x >= int(across) || c.y >= int(down)) continue;
        uint at = raw_store + (uint(c.y) * across + uint(c.x)) * VELK_PROBE_WORDS;
        if (uintBitsToFloat(probe_words.data[at + VELK_PROBE_FRAMES]) <= 0.0) continue;
        vec3 at_pos = velk_probe_position(at);
        vec3 at_n = velk_probe_normal(at);
        float off = max(abs(dot(n, at_pos - pos)), abs(dot(at_n, pos - at_pos)));
        float w = exp(-off / near) * pow(max(dot(n, at_n), 0.0), 4.0);
        if (w <= 1e-4) continue;
        VelkProbeLight q = velk_probe_read(at);
        light.e += w * velk_probe_eval_raw(q, at_n, n);
        light.r += w * q.r;
        light.g += w * q.g;
        light.b += w * q.b;
        total += w;
    }
    // Its own probe is always among them, with weight 1.
    light.e = max(light.e / total, vec3(0.0));
    light.r /= total;
    light.g /= total;
    light.b /= total;
    velk_probe_write(now, light, pos, n, frames);
}

// Lets go of the cell in slot `slot` when no probe read it for VELK_CELL_KEEP frames.
void velk_probe_cells_age(uint slot)
{
    if (slot >= VELK_CELL_SLOTS || pc.env.w == 0xFFFFFFFFu) return;
    uint cell = pc.env.w + slot * VELK_CELL_WORDS;
    if (probe_words.data[cell + VELK_CELL_KEY] == 0u) return;
    uint now = VELK_GLOBALS.present_counter;
    if (now - probe_words.data[cell + VELK_CELL_READ] <= VELK_CELL_KEEP) return;
    probe_words.data[cell + VELK_CELL_SAMPLES] = 0u;
    probe_words.data[cell + VELK_CELL_UPDATED] = 0u;
    probe_words.data[cell + VELK_CELL_CHECK] = 0u;
    probe_words.data[cell + VELK_CELL_KEY] = 0u;
}

void main()
{
    if (pc.extras.w == 1u) {
        velk_probe_cells_age(gl_WorkGroupID.x * 64u + gl_LocalInvocationIndex);
        return;
    }
    velk_probes_filter();
}
