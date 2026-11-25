const uint kSortMax = 2048u;
const uint kSortIndexBits = 11u;
const uint kSortIndexMask = 2047u;
shared uint s_order[kSortMax];

// The low 10 bits of `v` spread to every third bit.
uint velk_morton_spread(uint v)
{
    v &= 0x3FFu;
    v = (v | (v << 16u)) & 0x030000FFu;
    v = (v | (v << 8u)) & 0x0300F00Fu;
    v = (v | (v << 4u)) & 0x030C30C3u;
    v = (v | (v << 2u)) & 0x09249249u;
    return v;
}

// The 30-bit Morton code of `p` within the box `lo` to `hi`. One grid scale on all axes, the
// box's widest, so that a flat spread still gets cells that are square.
uint velk_morton(vec3 p, vec3 lo, vec3 hi)
{
    float extent = max(max(hi.x - lo.x, hi.y - lo.y), max(hi.z - lo.z, 1e-20));
    vec3 q = clamp((p - lo) / extent, 0.0, 1.0) * 1023.0;
    return (velk_morton_spread(uint(q.x)) << 2u) | (velk_morton_spread(uint(q.y)) << 1u) | velk_morton_spread(uint(q.z));
}

// Sorts s_order's first `n` keys ascending, `n` a power of two, the `lanes` threads of the work
// group together; all of them call it.
void velk_sort_keys(uint n, uint lane, uint lanes)
{
    for (uint k = 2u; k <= n; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            for (uint i = lane; i < n; i += lanes) {
                uint l = i ^ j;
                if (l > i) {
                    uint a = s_order[i];
                    uint b = s_order[l];
                    if ((a > b) == ((i & k) == 0u)) {
                        s_order[i] = b;
                        s_order[l] = a;
                    }
                }
            }
            memoryBarrierShared();
            barrier();
        }
    }
}
