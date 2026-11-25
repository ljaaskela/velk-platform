#define VELK_CELL_SLOTS 131072u
#define VELK_CELL_WORDS 12u
#define VELK_CELL_KEY 0u       // its key's hash, 0 for a free slot
#define VELK_CELL_CHECK 1u     // a second hash of its key
#define VELK_CELL_LIGHT 2u     // six words, as VELK_PROBE_LIGHT
#define VELK_CELL_SAMPLES 8u   // the rays' frames it holds, up to VELK_CELL_SETTLED
#define VELK_CELL_READ 9u      // the frame a probe last read it
#define VELK_CELL_UPDATED 10u  // the frame a probe last added its rays to it
#define VELK_CELL_NORMAL 11u   // the normal its light is at, octahedral
#define VELK_CELL_SETTLED 64u
#define VELK_CELL_WOKEN 4u
#define VELK_CELL_KEEP 240u
#define VELK_CELL_PROBES 8u

vec3 velk_cell_normal(uint cell)
{
    vec2 e = unpackSnorm2x16(VELK_LIGHT_WORDS.data[cell + VELK_CELL_NORMAL]);
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

uint velk_cell_hash(uvec3 v)
{
    uint h = (v.x * 0x8da6b343u) ^ (v.y * 0xd8163841u) ^ (v.z * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// The cell of a place at `pos` (relative to the camera) facing `n`: its key and second hash. A cell
// is a cube of the world a power of two wide, the widest no wider than a probe's square is at the
// place's distance (at least an eighth), and one of six ways a surface faces. `level` is its width
// as a power of two, from -3. At most as wide as the square, so that a cell has about one probe
// and gets as many rays as a probe while light changes.
void velk_cell_key_at(vec3 pos, vec3 n, int level, out uint key, out uint check)
{
    GlobalData g = VELK_GLOBALS;
    float size = exp2(float(level));
    // The camera's place in whole cells and what is left of it, so that far from the origin the
    // cell is found without adding a large number to a small one.
    vec3 cam = g.cam_pos.xyz;
    vec3 cam_cells = floor(cam / size);
    ivec3 c = ivec3(cam_cells) + ivec3(floor((pos + (cam - cam_cells * size)) / size));
    vec3 a = abs(n);
    uint face = a.x >= a.y && a.x >= a.z ? (n.x >= 0.0 ? 0u : 1u) : a.y >= a.z ? (n.y >= 0.0 ? 2u : 3u) : (n.z >= 0.0 ? 4u : 5u);
    uint level_face = (uint(level + 32) << 3u) | face;
    key = velk_cell_hash(uvec3(c) ^ uvec3(level_face * 0x9e3779b9u, level_face, 0u)) | 1u;
    check = velk_cell_hash(uvec3(c.yzx) + uvec3(level_face, 0x51ed270bu, level_face * 0x2545f491u));
}

void velk_cell_key(vec3 pos, vec3 n, out uint key, out uint check, out int level)
{
    GlobalData g = VELK_GLOBALS;
    mat4 vp = g.view_projection;
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
    float square = float(VELK_PROBE_PIXELS) * length(pos) / max(pixels_per_unit, 1e-6);
    level = min(int(floor(log2(max(square, 0.125)))), 27);
    velk_cell_key_at(pos, n, level, key, check);
}

// The cell with `key` and `check` in the table at word `table` that holds light, without taking
// a slot for it: 0xFFFFFFFF when there is none. All its slots are looked at, as one let go of
// (velk_probe_cells_age) leaves a free slot before cells that are still there.
uint velk_cell_lookup(uint table, uint key, uint check)
{
    for (uint p = 0u; p < VELK_CELL_PROBES; ++p) {
        uint cell = table + ((key + p) & (VELK_CELL_SLOTS - 1u)) * VELK_CELL_WORDS;
        uint k = VELK_LIGHT_WORDS.data[cell + VELK_CELL_KEY];
        if (k == key && VELK_LIGHT_WORDS.data[cell + VELK_CELL_CHECK] == check) {
            return VELK_LIGHT_WORDS.data[cell + VELK_CELL_SAMPLES] > 0u ? cell : 0xFFFFFFFFu;
        }
    }
    return 0xFFFFFFFFu;
}

// The cell holding light for a place at `pos` (relative to the camera) facing `n`, as wide as a
// probe there would take or the next width up: 0xFFFFFFFF when neither holds any.
uint velk_cell_light_at(uint table, vec3 pos, vec3 n)
{
    uint key;
    uint check;
    int level;
    velk_cell_key(pos, n, key, check, level);
    uint cell = velk_cell_lookup(table, key, check);
    if (cell != 0xFFFFFFFFu) return cell;
    velk_cell_key_at(pos, n, level + 1, key, check);
    return velk_cell_lookup(table, key, check);
}

// The cell with `key` and `check` in the table at word `table`, or a free slot taken for it:
// `claimed` when this call took it. 0xFFFFFFFF when its slots are all others'.
uint velk_cell_find(uint table, uint key, uint check, out bool claimed)
{
    claimed = false;
    for (uint p = 0u; p < VELK_CELL_PROBES; ++p) {
        uint cell = table + ((key + p) & (VELK_CELL_SLOTS - 1u)) * VELK_CELL_WORDS;
        uint k = VELK_LIGHT_WORDS.data[cell + VELK_CELL_KEY];
        if (k == 0u) {
            k = atomicCompSwap(VELK_LIGHT_WORDS.data[cell + VELK_CELL_KEY], 0u, key);
            if (k == 0u) {
                VELK_LIGHT_WORDS.data[cell + VELK_CELL_CHECK] = check;
                claimed = true;
                return cell;
            }
        }
        if (k == key && VELK_LIGHT_WORDS.data[cell + VELK_CELL_CHECK] == check) return cell;
    }
    return 0xFFFFFFFFu;
}
