layout(set = 1, binding = 16, std430) buffer ProbeWords { uint data[]; } probe_words;
#define VELK_LIGHT_WORDS probe_words
#define velk_light_cell_words probe_words
#define CELLS_HEADER pc.env.z
#velk_include VELK_LIGHT_POOL_GLSL
#define VELK_PROBE_PIXELS 16u
#define VELK_PROBE_RAYS 4u
#velk_include VELK_PROBE_GLSL
#velk_include VELK_PROBE_WRITE_GLSL
#velk_include VELK_PROBE_CELL_GLSL
uint velk_light_grid_hash(ivec3 cube)
{
    uint h = (uint(cube.x) * 0x8da6b343u) ^ (uint(cube.y) * 0xd8163841u) ^ (uint(cube.z) * 0xcb1ab31fu);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
uint velk_light_grid_count(vec3 world_at, out uint first)
{
    first = 0u;
    uint grid = probe_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return pc.light_count;
    uint buckets = probe_words.data[grid];
    float size = uintBitsToFloat(probe_words.data[grid + 1u]);
    uint everywhere = probe_words.data[grid + 2u];
    uint b = grid + 4u + everywhere + (velk_light_grid_hash(ivec3(floor(world_at / size))) & (buckets - 1u)) * 2u;
    uint count = probe_words.data[b + 1u];
    if (count != 0u) first = grid + probe_words.data[b];
    return everywhere + count;
}
uint velk_light_grid_at(uint first, uint k)
{
    uint grid = probe_words.data[CELLS_HEADER + 18u];
    if (grid == 0xFFFFFFFFu) return k;
    uint everywhere = probe_words.data[grid + 2u];
    return k < everywhere ? probe_words.data[grid + 4u + k] : probe_words.data[first + k - everywhere];
}
#velk_include VELK_LIGHT_SLOTS_GLSL
#velk_include VELK_LIGHT_ADD_GLSL
#velk_include VELK_LIGHT_EVAL_GLSL
#velk_include VELK_LIGHT_AREA_GLSL
// Each draw record's shading summary (kDrawShadeWords, gpu_data.h), at the record's index.
layout(set = 1, binding = 6, std430) readonly buffer VelkDrawShade { uint data[]; } velk_draw_shade;
#define VELK_DRAW_SHADE_WORDS 3u
#define VELK_DRAW_SHADE_UNLIT 0x01000000u
#define VELK_DRAW_SHADE_TINTED 0x02000000u
#define VELK_DRAW_SHADE_METALLIC_SHIFT 26u

vec3 velk_srgb_to_linear(vec3 s)
{
    return mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), greaterThan(s, vec3(0.04045)));
}

// What a ray sees of the surface it hit, without its material's code: its draw's shading
// summary, its albedo times, where its material takes them, its instance's colour and its vertex
// colours, and on a quad the quad's colour. A lit surface reflects that albedo less its metallic
// diffusely (`albedo`), and its metal reflects the rest tinted by it (`metal`, for what it
// reflects from the way a ray would mirror off it); an unlit one gives it off, on top of its
// emission. A lit surface of a double-sided draw passes `transmission` of its diffuse light from
// behind it, as the deferred lighting has it; 0 otherwise. White, lit, not metal and passing
// nothing where the hit names no draw.
void velk_probe_surface(RayHit hit, out vec3 albedo, out vec3 metal, out vec3 emission, out bool lit, out float transmission)
{
    albedo = vec3(1.0);
    metal = vec3(0.0);
    emission = vec3(0.0);
    lit = true;
    transmission = 0.0;
    if (hit.draw == 0xFFFFFFFFu || hit.instance == 0xFFFFFFFFu) return;
    uint at = hit.draw * VELK_DRAW_SHADE_WORDS;
    uint look = 0x00FFFFFFu | VELK_DRAW_SHADE_TINTED;
    uint glow = 0u;
    if (at + 2u < uint(velk_draw_shade.data.length())) {
        look = velk_draw_shade.data[at];
        glow = velk_draw_shade.data[at + 1u];
        transmission = clamp(uintBitsToFloat(velk_draw_shade.data[at + 2u]), 0.0, 1.0);
    }
    // The draw record only where something passes: only a double-sided draw lets it through.
    if (transmission > 0.0 && (velk_draw_data.data[hit.draw].flags & VELK_DRAW_DOUBLE_SIDED) == 0u) transmission = 0.0;
    // The draw record only where the tint is taken: quads have no material, so they are tinted.
    vec4 tint = vec4(1.0);
    if ((look & VELK_DRAW_SHADE_TINTED) != 0u) {
        VelkDrawData d = velk_draw_data.data[hit.draw];
        if ((d.flags & VELK_DRAW_QUADS) != 0u) {
            // A quad's hit names the quad's word, whose colour is what the quad shows.
            tint = unpackUnorm4x8(velk_culling.data[hit.instance + VELK_QUAD_NODE_COLOR]);
        } else {
            tint = velk_instances.data[hit.instance].color;
            if ((d.flags & VELK_DRAW_COLOR) != 0u && hit.corners.x != VELK_NO_CORNERS) {
                vec3 b = vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights.x, hit.weights.y);
                tint *= velk_read_color(velk_cluster_color_word(d, hit.cluster, hit.corners.x)) * b.x
                      + velk_read_color(velk_cluster_color_word(d, hit.cluster, hit.corners.y)) * b.y
                      + velk_read_color(velk_cluster_color_word(d, hit.cluster, hit.corners.z)) * b.z;
            }
        }
    }
    vec3 surface = velk_srgb_to_linear(unpackUnorm4x8(look).rgb) * tint.rgb;
    lit = (look & VELK_DRAW_SHADE_UNLIT) == 0u;
    if (!lit) transmission = 0.0;
    float metallic = float(look >> VELK_DRAW_SHADE_METALLIC_SHIFT) / 63.0;
    albedo = lit ? surface * (1.0 - metallic) : vec3(0.0);
    // As F0 is, the dielectric base reflecting 4% of what is not metal.
    metal = lit ? mix(vec3(0.04), surface, metallic) : vec3(0.0);
    emission = velk_rgb9e5(glow) + (lit ? vec3(0.0) : surface);
}


// What each ray found and which way it went, for its probe's first thread.
shared vec3 s_light[VELK_RT_GROUP_THREADS];
shared vec3 s_dir[VELK_RT_GROUP_THREADS];
// The deepest any thread's walk stack got, for one write to the log's word a group.
shared uint s_deepest;
// A probe's rays alone, its cell read without rays, or its rays added to its cell.
#define VELK_CELL_OWN 0u
#define VELK_CELL_READ_ONLY 1u
#define VELK_CELL_ADD 2u

// Whether a change in the change list at word `changes` (light_changes_main_src's, VELK_CHANGE_LIST)
// could alter the light bouncing to a cell `level` wide (velk_cell_key) at `pos` (relative to the
// camera): one that wakes cells that wide (VELK_CHANGE_WAKES), and is a light that reaches half
// again as far as its range (its light reaches what bounces it here), or any light without a
// range, or a box within its own size (what moved shadows and lights its surroundings by about as
// much).
bool velk_cell_changed(uint changes, vec3 pos, int level)
{
    if (changes == 0xFFFFFFFFu) return false;
    vec3 cam = VELK_GLOBALS.cam_pos.xyz;
    uint bit = 1u << uint(clamp(level + 3, 0, 31));
    uint count = min(probe_words.data[changes + VELK_CHANGE_LIST_COUNT], 128u);
    for (uint k = 0u; k < count; ++k) {
        uint w = changes + VELK_CHANGE_LIST_CHANGES + k * VELK_CHANGE_WORDS;
        if ((probe_words.data[w + VELK_CHANGE_WAKES] & bit) == 0u) continue;
        vec3 lo = vec3(uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MIN]), uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MIN + 1u]),
                       uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MIN + 2u])) - cam;
        vec3 hi = vec3(uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MAX]), uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MAX + 1u]),
                       uintBitsToFloat(probe_words.data[w + VELK_CHANGE_MAX + 2u])) - cam;
        uint word = probe_words.data[w + VELK_CHANGE_LIGHT];
        if (word != 0xFFFFFFFFu) {
            // Its light, without the small move's mark (kLightChangeSmall).
            float range = velk_light_at(pc.lights_base + (word & ~0x40000000u)).params.x;
            if (range <= 0.0 || distance(pos, lo) <= 1.5 * range) return true;
            continue;
        }
        if (distance(pos, clamp(pos, lo, hi)) <= max(length(hi - lo), 1.0)) return true;
    }
    return false;
}

// What light `index` brings a surface at `at` facing `n`, seen along `to_viewer`, unshadowed; a
// stand-in's as velk_diffuse_response has it (`stand_in` its flatness, below 0 for none). An area
// light's in closed form, as the deferred lighting has it, stand-ins included. A thin sheet passing
// `transmission` of its diffuse light takes that share from behind it, as the deferred lighting
// has it: a stand-in's delta lights none.
vec3 velk_probe_light(uint index, vec3 at, vec3 n, float stand_in, vec3 to_viewer, float transmission)
{
    Light light = velk_light_at(pc.lights_base + index);
    if (light.flags.x == 3u) {
        float half_size = max(light.params.w, 1e-4);
        float diffuse = velk_area_diffuse(light.position.xyz, light.direction.xyz, half_size, at, n) * (1.0 - transmission);
        if (transmission > 0.0) {
            diffuse += velk_area_diffuse(light.position.xyz, light.direction.xyz, half_size, at, -n) * transmission;
        }
        return light.color_intensity.rgb * light.color_intensity.a * diffuse;
    }
    vec3 l;
    float atten;
    velk_light_delta(light, at, l, atten);
    float diffuse = stand_in < 0.0 && transmission > 0.0 ? mix(max(dot(n, l), 0.0), max(-dot(n, l), 0.0), transmission)
                                                         : velk_diffuse_response(n, l, to_viewer, stand_in);
    return light.color_intensity.rgb * light.color_intensity.a * atten * diffuse;
}

// Where a hit at `at` keeps its light: the word of its light, of its radiance and of the way the
// pixels that gave that face (a record's only; a quad faces one way), and the place its one
// value's lights are chosen at. Its record's, at the hit; for a quad, the node as wide as
// kLightPixels over it, at the node's centre, as the lighting pass finds it. All VELK_LIGHT_NONE
// for none. `ask` is whether a hit may ask for its one value to be traced (velk_probe_ask): a
// record for rays only (kCutForRays), never drawn, has no light but the one value hits ask for.
void velk_probe_hit_words(RayHit hit, vec3 at, out uint light, out uint radiance, out uint radiance_dir, out vec3 center,
                          out bool ask)
{
    light = VELK_LIGHT_NONE;
    radiance = VELK_LIGHT_NONE;
    radiance_dir = VELK_LIGHT_NONE;
    center = at;
    ask = false;
    if (hit.record == VELK_NO_RECORD) return;
    uint entry = probe_words.data[hit.record + VELK_RECORD_ENTRY];
    if (entry == VELK_CUT_QUAD) {
        GlobalData g = VELK_GLOBALS;
        mat4 vp = g.view_projection;
        float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5;
        uint node = velk_light_quad_node(probe_words.data[hit.record + VELK_RECORD_QUAD_TREE],
                                         probe_words.data[hit.record + VELK_RECORD_QUAD_NODE], g.cam_pos.xyz, pixels_per_unit,
                                         VELK_LIGHT_PIXELS);
        light = node + VELK_QUAD_NODE_LIGHT;
        radiance = node + VELK_QUAD_NODE_RADIANCE;
        center = vec3(uintBitsToFloat(probe_words.data[node + VELK_QUAD_NODE_CENTER]),
                      uintBitsToFloat(probe_words.data[node + VELK_QUAD_NODE_CENTER + 1u]),
                      uintBitsToFloat(probe_words.data[node + VELK_QUAD_NODE_CENTER + 2u])) - g.cam_pos.xyz;
        ask = true;
        return;
    }
    if (entry == VELK_CUT_RAY_QUAD || entry == VELK_CUT_STALE) return;
    light = hit.record + VELK_RECORD_LIGHT;
    // Kept indirect light only on a cluster, which bounds it (velk_light_direct).
    if (entry != 0xFFFFFFFFu) {
        radiance = hit.record + VELK_RECORD_RADIANCE;
        radiance_dir = hit.record + VELK_RECORD_RADIANCE_DIR;
    }
    ask = true;
}

// Asks for the light at `word` to be traced, for a hit: none yet becomes an untraced one value,
// and an untraced one value is marked asked, which the next frame's mark lists for tracing
// (light_mark_src). Light with pages, or being given them, is left as it is.
void velk_probe_ask(uint word)
{
    uint value = probe_words.data[word];
    if (value == VELK_LIGHT_NONE) {
        atomicCompSwap(probe_words.data[word], VELK_LIGHT_NONE, VELK_LIGHT_ONE_VALUE | VELK_LIGHT_ONE_ASKED);
        return;
    }
    if (value < VELK_LIGHT_ONE_VALUE || value >= VELK_LIGHT_PENDING) return;
    if ((value & (VELK_LIGHT_ONE_TRACED | VELK_LIGHT_ONE_ASKED)) == 0u) atomicOr(probe_words.data[word], VELK_LIGHT_ONE_ASKED);
}

// The direct light a surface at `at`, facing `n`, gets as its light points there say, for a hit
// whose light is at `word`: its one value, its lights chosen at `center`, or its record's points:
// down its triangle's pieces where the hit knows its triangle (RayHit::triangle,
// velk_light_add_points), else at its triangle's corners; each light by how much of it gets
// through. `to_viewer` points back along the ray that hit it, for a stand-in (RayHit::stand_in).
// A thin sheet passing `transmission` takes that share from behind it (velk_probe_light).
vec3 velk_probe_direct(RayHit hit, uint word, vec3 center, vec3 at, vec3 n, vec3 to_viewer, float transmission)
{
    uint pool = pc.shapes_base;
    if (word == VELK_LIGHT_NONE || pool == 0xFFFFFFFFu) return vec3(0.0);
    uint value = probe_words.data[word];
    if (value >= VELK_LIGHT_PENDING) return vec3(0.0);
    uint lights[VELK_LIGHT_SLOTS];
    vec2 sums[VELK_LIGHT_SLOTS];
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        lights[k] = VELK_LIGHT_NONE;
        sums[k] = vec2(0.0);
    }
    if (value >= VELK_LIGHT_ONE_VALUE) {
        if ((value & VELK_LIGHT_ONE_TRACED) == 0u) return vec3(0.0);
        velk_light_add_one(value, center, lights, sums);
    } else {
        if (value >= probe_words.data[pool + VELK_POOL_PAGES] || hit.corners.x == VELK_NO_CORNERS) return vec3(0.0);
        vec3 b = vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights.x, hit.weights.y);
        // Where the hit knows its triangle: down its pieces to the deepest whose points around
        // it are traced and fresh (velk_light_add_points). Else its triangle's corners.
        uint first = velk_light_page(pool, value);
        uint t = hit.triangle != VELK_NO_CORNERS ? hit.triangle : 0xFFFFFFFFu;
        if (!velk_light_add_points(pool, first, t, hit.corners, b, at, 0.0, 0.0, lights, sums)) {
            return vec3(0.0);
        }
    }
    vec3 total = vec3(0.0);
    for (uint k = 0u; k < VELK_LIGHT_SLOTS; ++k) {
        if (lights[k] == VELK_LIGHT_NONE) continue;
        total += velk_probe_light(lights[k], at, n, hit.stand_in, to_viewer, transmission) * (sums[k].y / max(sums[k].x, 1e-6));
    }
    return total;
}

// The environment along `rd`, `blur` of the way (0 to 1) from its sharpest mip level to its
// blurriest. Black without one.
vec3 velk_probe_env(vec3 rd, float blur)
{
    if (pc.env.y == 0u) return vec3(0.0);
    float c = cos(pc.env_params.y);
    float s = sin(pc.env_params.y);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    float last = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env.y)])) - 1.0;
    return textureLod(velk_textures[nonuniformEXT(pc.env.y)], uv, clamp(blur, 0.0, 1.0) * last).rgb * pc.env_params.x;
}

// The environment along `rd`, as a probe's miss sees it: its blurriest level.
vec3 velk_probe_sky(vec3 rd)
{
    return velk_probe_env(rd, 1.0);
}

// The indirect light a hit at `at` facing `n` gets, as the lighting pass would light it: its
// probes' cell in the table at word `cells` (VELK_PROBE_CELL_GLSL), as wide as a probe there takes
// or the next width up, so that light bounces once more each frame and stays where the camera
// left it; else the last frame's probe where the hit was on screen, when that probe was on the
// same surface; else what its record or quad node keeps at `radiance` from the pixels that showed
// it, where those faced about as it does (`radiance_dir`, their way on average, VELK_LIGHT_NONE
// for a quad's); else the environment around its normal, unshadowed. A record seen all round
// averages to a short way, which no hit agrees with.
vec3 velk_probe_hit_indirect(uint cells, vec3 at, vec3 n, uint radiance, uint radiance_dir, uint width, uint height, uint across)
{
    GlobalData g = VELK_GLOBALS;
    if (cells != 0xFFFFFFFFu) {
        uint cell = velk_cell_light_at(cells, at, n);
        if (cell != 0xFFFFFFFFu) return velk_probe_eval(velk_probe_read(cell + VELK_CELL_LIGHT), velk_cell_normal(cell), n);
    }
    uint was_store = pc.env.w;
    vec4 clip = g.prev_view_projection * vec4(at + g.cam_moved, 1.0);
    if (was_store != 0xFFFFFFFFu && clip.w > 1e-5) {
        vec2 p = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(width), float(height));
        if (all(greaterThanEqual(p, vec2(0.0))) && p.x < float(width) && p.y < float(height)) {
            uint before = was_store + ((uint(p.y) / VELK_PROBE_PIXELS) * across + uint(p.x) / VELK_PROBE_PIXELS) * VELK_PROBE_WORDS;
            if (uintBitsToFloat(probe_words.data[before + VELK_PROBE_FRAMES]) > 0.0) {
                vec3 then_pos = velk_probe_position(before) - g.cam_moved;
                vec3 then_n = velk_probe_normal(before);
                if (dot(then_n, n) > 0.9 && abs(dot(then_n, at - then_pos)) < 0.05 * length(at) + 1e-3) {
                    return velk_probe_eval(velk_probe_read(before), then_n, n);
                }
            }
        }
    }
    if (radiance != VELK_LIGHT_NONE) {
        uint kept = probe_words.data[radiance];
        bool facing = radiance_dir == VELK_LIGHT_NONE || dot(n, unpackSnorm4x8(probe_words.data[radiance_dir]).xyz) > 0.5;
        if (kept != VELK_LIGHT_NONE && facing) return velk_rgb9e5(kept);
    }
    return velk_probe_sky(n);
}

// The direction of ray `ray` of probe `probe`, at `square` on the probe grid, in frame `frame`,
// around the normal `n`: over the hemisphere by cosine, in a cell of a 4 by 4 grid of directions
// (the squared sine to the normal one way, the turn about it the other). Each frame a probe's four
// rays take one band each way, each of the four rows and each of the four columns; over four
// frames a probe takes all sixteen cells, and every 2 by 2 block of probes takes all sixteen each
// frame. The place in the cell is the probe's own and moves every four frames.
vec3 velk_probe_ray_dir(uint probe, uvec2 square, uint ray, uint frame, vec3 n)
{
    uint s = (frame & 3u) ^ ((square.x & 1u) | ((square.y & 1u) << 1u));
    vec2 cell = vec2(float(ray), float(((ray & 1u) * 2u + (ray >> 1u)) ^ s));
    vec2 jitter = fract(vec2(float(probe) * 0.7548777, float(probe) * 0.5698403)
                        + float(frame >> 2u) * vec2(0.6180340, 0.3819660));
    vec2 uv = (cell + jitter) * 0.25;
    // By cosine: as many rays as the light they carry, few grazing.
    float z = sqrt(max(1.0 - uv.x, 0.0));
    float r = sqrt(uv.x);
    float phi = 6.2831853 * uv.y;
    vec3 a = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, a);
    return normalize(a * (r * cos(phi)) + b * (r * sin(phi)) + n * z);
}


// The probe list's words (kProbeListHeaderWords, kProbeListEntryWords): how many probes cast rays,
// the trace's dispatch arguments (a group for each four), then each probe's: its index, with its
// cell's samples before (bits 24 to 30) and bit 31 when its rays go into its cell; and the cell.
#define VELK_PROBE_LIST_COUNT 0u
#define VELK_PROBE_LIST_ARGS 1u
#define VELK_PROBE_LIST_ENTRIES 4u
#define VELK_PROBE_LIST_ADD 0x80000000u

// A probe's surface, the middle pixel of its square at `square`: false where there is none.
bool velk_probe_place(uvec2 square, uint width, uint height, out vec3 pos, out vec3 n)
{
    ivec2 pixel = ivec2(int(min(square.x * VELK_PROBE_PIXELS + VELK_PROBE_PIXELS / 2u, width - 1u)),
                        int(min(square.y * VELK_PROBE_PIXELS + VELK_PROBE_PIXELS / 2u, height - 1u)));
    pos = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], pixel, 0).xyz;
    vec3 n4 = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], pixel, 0).xyz;
    n = dot(n4, n4) > 1e-6 ? normalize(n4) : vec3(0.0, 1.0, 0.0);
    return dot(n4, n4) > 1e-6;
}

// What each probe does this frame, a thread a probe, before the trace (velk_probes_trace): with
// no surface, nothing; its cell (pc.more.x the table, pc.more.y the frame everything last changed,
// pc.more.z this frame's change list) settled and nothing changed since, read without rays; else
// it adds its rays to it unless another does this frame, which leaves it to be read, or casts its
// rays alone while it has none yet. A change near it (velk_cell_changed), or everything changing,
// starts a settled cell moving again from VELK_CELL_WOKEN samples. The probes that cast rays go to
// the list at pc.more.w, so that the trace's groups are all of probes that do.
void velk_probes_decide(uint probe)
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint down = (height + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    if (probe >= across * down) return;
    uvec2 square = uvec2(probe % across, probe / across);
    uint raw = pc.extras.x + probe * VELK_PROBE_WORDS;
    vec3 pos;
    vec3 n;
    if (!velk_probe_place(square, width, height, pos, n)) {
        for (uint w = 0u; w < VELK_PROBE_WORDS; ++w) probe_words.data[raw + w] = 0u;
        return;
    }
    uint mode = VELK_CELL_OWN;
    uint cell = 0xFFFFFFFFu;
    uint had = 0u;
    if (pc.more.x != 0xFFFFFFFFu) {
        uint key;
        uint check;
        int level;
        velk_cell_key(pos, n, key, check, level);
        bool claimed;
        cell = velk_cell_find(pc.more.x, key, check, claimed);
        if (cell != 0xFFFFFFFFu) {
            uint now = VELK_GLOBALS.present_counter;
            probe_words.data[cell + VELK_CELL_READ] = now;
            uint samples = claimed ? 0u : probe_words.data[cell + VELK_CELL_SAMPLES];
            uint updated = probe_words.data[cell + VELK_CELL_UPDATED];
            bool woken = updated < pc.more.y || velk_cell_changed(pc.more.z, pos, level);
            had = woken ? min(samples, VELK_CELL_WOKEN) : samples;
            if (samples >= VELK_CELL_SETTLED && !woken) {
                mode = VELK_CELL_READ_ONLY;
            } else if (updated != now && atomicCompSwap(probe_words.data[cell + VELK_CELL_UPDATED], updated, now) == updated) {
                mode = VELK_CELL_ADD;
            } else if (samples > 0u) {
                mode = VELK_CELL_READ_ONLY;
            }
            if (woken && samples >= VELK_CELL_SETTLED && pc.shapes_base != 0xFFFFFFFFu) {
                atomicAdd(probe_words.data[pc.shapes_base + VELK_POOL_SUM_PROBES_READ + 3u], 1u);
            }
        }
    }
    if (pc.shapes_base != 0xFFFFFFFFu) {
        atomicAdd(probe_words.data[pc.shapes_base + VELK_POOL_SUM_PROBES_READ + (mode == VELK_CELL_READ_ONLY ? 0u : mode == VELK_CELL_ADD ? 1u : 2u)], 1u);
    }
    if (mode == VELK_CELL_READ_ONLY) {
        velk_probe_write(raw, velk_probe_read(cell + VELK_CELL_LIGHT), pos, velk_cell_normal(cell),
                         float(max(probe_words.data[cell + VELK_CELL_SAMPLES], 1u)));
        return;
    }
    uint list = pc.more.w & 0x7FFFFFFFu;
    uint slot = atomicAdd(probe_words.data[list + VELK_PROBE_LIST_COUNT], 1u);
    if (slot % 4u == 0u) atomicAdd(probe_words.data[list + VELK_PROBE_LIST_ARGS], 1u);
    uint e = list + VELK_PROBE_LIST_ENTRIES + slot * 2u;
    probe_words.data[e] = probe | (min(had, 127u) << 24u) | (mode == VELK_CELL_ADD ? VELK_PROBE_LIST_ADD : 0u);
    probe_words.data[e + 1u] = cell;
}

// The trace: four probes of the list a group of 16 threads, a ray a thread, each probe's light
// from its rays into the ray store, and into its cell where it adds to it.
void velk_probes_trace(uint lane)
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uint list = pc.more.w & 0x7FFFFFFFu;
    uint local = lane / VELK_PROBE_RAYS;
    uint ray = lane % VELK_PROBE_RAYS;
    uint slot = gl_WorkGroupID.x * 4u + local;
    bool live = slot < probe_words.data[list + VELK_PROBE_LIST_COUNT];
    uint entry = live ? probe_words.data[list + VELK_PROBE_LIST_ENTRIES + slot * 2u] : 0u;
    uint cell = live ? probe_words.data[list + VELK_PROBE_LIST_ENTRIES + slot * 2u + 1u] : 0xFFFFFFFFu;
    uint probe = entry & 0xFFFFFFu;
    uint had = (entry >> 24u) & 0x7Fu;
    bool add = (entry & VELK_PROBE_LIST_ADD) != 0u;
    uvec2 square = uvec2(probe % across, probe / across);
    uint raw = pc.extras.x + probe * VELK_PROBE_WORDS;
    GlobalData g = VELK_GLOBALS;
    vec3 pos = vec3(0.0);
    vec3 n = vec3(0.0, 1.0, 0.0);
    if (live) live = velk_probe_place(square, width, height, pos, n);

    if (lane == 0u) s_deepest = 0u;
    memoryBarrierShared();
    barrier();

    vec3 light = vec3(0.0);
    vec3 dir = n;
    if (live) {
        dir = velk_probe_ray_dir(probe, square, ray, g.present_counter, n);
        Ray rr;
        rr.origin = pos + n * max(length(pos) * 1e-3, 1e-3);
        rr.dir = dir;
        RayHit hit;
        if (trace_closest_hit(rr, 1e30, hit)) {
            vec3 at = rr.origin + dir * hit.t;
            vec3 hn = normalize(hit.normal);
            if (dot(hn, dir) > 0.0) hn = -hn;
            vec3 albedo;
            vec3 metal;
            vec3 emission;
            bool lit;
            float transmission;
            velk_probe_surface(hit, albedo, metal, emission, lit, transmission);
            uint light_word;
            uint radiance;
            uint radiance_dir;
            vec3 center;
            bool ask;
            velk_probe_hit_words(hit, at, light_word, radiance, radiance_dir, center, ask);
            if (lit && ask && pc.shapes_base != 0xFFFFFFFFu) velk_probe_ask(light_word);
            // As the lighting pass lights a pixel: albedo times each light's N.L and strength, and
            // its indirect light.
            vec3 indirect = lit ? velk_probe_hit_indirect(pc.more.x, at, hn, radiance, radiance_dir, width, height, across) : vec3(0.0);
            light = albedo * (velk_probe_direct(hit, light_word, center, at, hn, -dir, transmission) + indirect) + emission;
        } else {
            light = velk_probe_sky(dir);
        }
    }
    s_light[lane] = light;
    s_dir[lane] = dir;
    // The deepest the walk's stack got, for the log (light pool header 101): the group's deepest,
    // written once.
    if (velk_ray_deepest > 0) atomicMax(s_deepest, uint(velk_ray_deepest));
    memoryBarrierShared();
    barrier();
    if (lane == 0u && pc.shapes_base != 0xFFFFFFFFu && s_deepest > 0u) {
        atomicMax(probe_words.data[pc.shapes_base + VELK_POOL_DEEPEST_PROBES], s_deepest);
    }
    if (!live || ray != 0u) return;

    // By cosine: the mean radiance weighted by cosine, as the lighting pass takes it, is the mean
    // of what the rays found. Each colour's direction: along the normal one and a half times
    // that, across it four times the mean of what a ray found times its direction's part across
    // the normal, which for light changing evenly over the hemisphere is its L1 direction.
    VelkProbeLight p;
    p.e = vec3(0.0);
    p.r = vec3(0.0);
    p.g = vec3(0.0);
    p.b = vec3(0.0);
    for (uint k = 0u; k < VELK_PROBE_RAYS; ++k) {
        vec3 l = s_light[lane + k];
        vec3 w = s_dir[lane + k];
        vec3 across_n = w - dot(w, n) * n;
        p.e += l;
        p.r += l.r * across_n;
        p.g += l.g * across_n;
        p.b += l.b * across_n;
    }
    p.e /= float(VELK_PROBE_RAYS);
    p.r = 1.5 * p.e.r * n + p.r * (4.0 / float(VELK_PROBE_RAYS));
    p.g = 1.5 * p.e.g * n + p.g * (4.0 / float(VELK_PROBE_RAYS));
    p.b = 1.5 * p.e.b * n + p.b * (4.0 / float(VELK_PROBE_RAYS));
    if (add) {
        // Into its cell, at the cell's normal: the mean of all the rays it holds, up to
        // VELK_CELL_SETTLED frames of them; its count written last, so that a probe reading it
        // finds the light it counts.
        vec3 cn = had == 0u ? n : velk_cell_normal(cell);
        p.e = velk_probe_eval_raw(p, n, cn);
        if (had > 0u) {
            VelkProbeLight c = velk_probe_read(cell + VELK_CELL_LIGHT);
            float w = 1.0 / float(had + 1u);
            p.e = mix(c.e, p.e, w);
            p.r = mix(c.r, p.r, w);
            p.g = mix(c.g, p.g, w);
            p.b = mix(c.b, p.b, w);
        } else {
            probe_words.data[cell + VELK_CELL_NORMAL] = velk_probe_oct_encode(cn);
        }
        velk_probe_write_light(cell + VELK_CELL_LIGHT, p);
        memoryBarrierBuffer();
        uint samples = min(had + 1u, VELK_CELL_SETTLED);
        probe_words.data[cell + VELK_CELL_SAMPLES] = samples;
        velk_probe_write(raw, p, pos, cn, float(samples));
        return;
    }
    velk_probe_write(raw, p, pos, n, 1.0);
}


#ifdef VELK_REFLECT
// Roughness from which a surface reflects nothing traced (the lighting's own lobe instead), and
// below which its reflection is a mirror's.
#define VELK_REFLECT_CUTOFF 0.3
#define VELK_REFLECT_SHARP 0.05

// Reflections, as emit_reflection_pass runs this shader with VELK_REFLECT: a thread a 2 by 2
// block of pixels, a group 8 by 4 blocks, and each frame one pixel of each block, the next of
// the four the next frame. A lit pixel smoother than VELK_REFLECT_CUTOFF sends a ray along its
// reflection, the mirror one below VELK_REFLECT_SHARP, else one of its GGX lobe. What the ray
// hits is lit as a probe's ray finds it (velk_probe_surface, velk_probe_direct,
// velk_probe_hit_indirect), and its metal reflects the environment; a miss is the environment,
// as sharp as the reflection is. Into the samples image (pc.extras.x), a texel a block, every one
// written each frame: the light, and how far the ray went, -1 for a miss, 0 for no reflection.
// pc.extras.y is the G-buffer's size (x low, y high), z its positions, w its normals, pc.more.x
// its material parameters.
void velk_reflect_trace(uint lane)
{
    uint width = pc.extras.y & 0xFFFFu;
    uint height = pc.extras.y >> 16u;
    uint across = (width + VELK_PROBE_PIXELS - 1u) / VELK_PROBE_PIXELS;
    uvec2 blocks = uvec2((width + 1u) / 2u, (height + 1u) / 2u);
    uint groups_across = (blocks.x + 7u) / 8u;
    uvec2 block = uvec2((gl_WorkGroupID.x % groups_across) * 8u + lane % 8u, (gl_WorkGroupID.x / groups_across) * 4u + lane / 8u);
    if (block.x >= blocks.x || block.y >= blocks.y) return;
    GlobalData g = VELK_GLOBALS;
    uint k = g.present_counter & 3u;
    ivec2 pixel = min(ivec2(block * 2u + uvec2(k & 1u, k >> 1u)), ivec2(int(width) - 1, int(height) - 1));
    vec4 mat = texelFetch(velk_textures[nonuniformEXT(pc.more.x)], pixel, 0);
    float roughness = clamp(mat.g, 0.04, 1.0);
    uint mode = uint(mat.b * 255.0 + 0.5);
    vec3 n4 = texelFetch(velk_textures[nonuniformEXT(pc.extras.w)], pixel, 0).xyz;
    if (mode == 0u || dot(n4, n4) < 1e-6 || roughness >= VELK_REFLECT_CUTOFF) {
        imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.x)], ivec2(block), vec4(0.0));
        return;
    }
    vec3 pos = texelFetch(velk_textures[nonuniformEXT(pc.extras.z)], pixel, 0).xyz;
    vec3 n = normalize(n4);
    vec3 v = -normalize(pos);
    vec3 dir = reflect(-v, n);
    if (roughness >= VELK_REFLECT_SHARP) {
        rng_init(uvec2(pixel), g.present_counter);
        vec3 sampled = ggx_sample_reflect(v, n, roughness, rng_next_vec2());
        if (dot(sampled, n) > 0.0) dir = sampled;
    }
    Ray rr;
    rr.origin = pos + n * max(length(pos) * 1e-3, 1e-3);
    rr.dir = dir;
    RayHit hit;
    vec3 light = vec3(0.0);
    float t = -1.0;
    if (trace_closest_hit(rr, 1e30, hit)) {
        vec3 at = rr.origin + dir * hit.t;
        vec3 hn = normalize(hit.normal);
        if (dot(hn, dir) > 0.0) hn = -hn;
        vec3 albedo;
        vec3 metal;
        vec3 emission;
        bool lit;
        float transmission;
        velk_probe_surface(hit, albedo, metal, emission, lit, transmission);
        uint light_word;
        uint radiance;
        uint radiance_dir;
        vec3 center;
        bool ask;
        velk_probe_hit_words(hit, at, light_word, radiance, radiance_dir, center, ask);
        if (lit && ask && pc.shapes_base != 0xFFFFFFFFu) velk_probe_ask(light_word);
        vec3 indirect = lit ? velk_probe_hit_indirect(pc.more.w, at, hn, radiance, radiance_dir, width, height, across) : vec3(0.0);
        // Its metal: what it mirrors of the environment, half blurred, its own roughness not known.
        light = albedo * (velk_probe_direct(hit, light_word, center, at, hn, -dir, transmission) + indirect)
              + emission
              + metal * velk_probe_env(reflect(dir, hn), 0.5);
        // Within what half floats hold.
        t = clamp(hit.t, 1e-4, 65000.0);
    } else {
        // As the lighting blurs the environment for a lobe; a sampled ray is one of the lobe already.
        light = velk_probe_env(dir, roughness < VELK_REFLECT_SHARP ? roughness : 0.0);
    }
    imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.x)], ivec2(block), vec4(min(light, vec3(65000.0)), t));
}
#endif

void main()
{
#ifdef VELK_REFLECT
    velk_reflect_trace(gl_LocalInvocationIndex);
#else
    if ((pc.more.w & 0x80000000u) != 0u) {
        velk_probes_trace(gl_LocalInvocationIndex);
    } else {
        velk_probes_decide(gl_WorkGroupID.x * gl_WorkGroupSize.x + gl_LocalInvocationIndex);
    }
#endif
}
