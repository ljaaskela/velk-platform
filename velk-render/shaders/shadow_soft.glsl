// What the softening of shadows shares, for shaders built on the deferred prelude: the sun pass's
// (sun_soften_main_src) and the soft shadows' from the points of light (shadow_soften_main_src).

// Taps a disk each, its blocker search's reach and the widest a penumbra may be, in pixels.
const uint kSunTaps = 16u;
const float kSunSearchPixels = 32.0;
const float kSunPenumbraPixels = 64.0;

// Whether the pixel at `q` is on a surface like the one at `p` facing `n`.
bool velk_sun_alike(ivec2 q, vec3 p, vec3 n, out vec3 pq)
{
    vec3 nq = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], q, 0).xyz;
    pq = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], q, 0).xyz;
    if (dot(nq, nq) < 1e-6) return false;
    if (dot(normalize(nq), n) < 0.7) return false;
    return abs(dot(n, pq - p)) < 0.05 * length(pq - p) + 0.002 * length(p);
}

// Tap `k` of kSunTaps of a disk `radius` pixels wide around `c`, turned by `turn`.
ivec2 velk_sun_tap(ivec2 c, uint k, float radius, float turn)
{
    float r = sqrt((float(k) + 0.5) / float(kSunTaps)) * radius;
    float a = float(k) * 2.39996323 + turn;
    return c + ivec2(round(vec2(cos(a), sin(a)) * r));
}

// Where in the screen, in pixels, the point `u` off `p` perpendicular to the light `L` lands when
// carried along the light onto the plane through `p` facing `n` (`lean` its N.L, away from zero):
// false behind the camera.
bool velk_sun_land(vec3 p, vec3 n, vec3 L, float lean, vec3 u, mat4 vp, out vec2 px)
{
    vec4 clip = vp * vec4(p + u - L * (dot(n, u) / lean), 1.0);
    px = vec2(0.0);
    if (!(clip.w > 1e-6)) return false;
    px = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    return true;
}

// The soft shadows' words in the culling arena (DeferredPath::emit_shadow_soft_passes), from their
// first word: a header; then for each pixel, row by row, VELK_SHADOW_LIGHTS words, the lights the
// softening takes of it (velk_shadow_pack), VELK_LIGHT_NONE for none; then a word a pixel, what
// of each of them the softening found gets through, 8 bits each from the lowest, 0 to 254 its
// 254ths, VELK_SHADOW_KEEP for as the points of light say; then a word for each tile of
// VELK_SHADOW_TILE by VELK_SHADOW_TILE pixels, row by row, whether a light it took is kept from
// some of it (VELK_SHADOW_TILE_SHADOW) and reaches some (VELK_SHADOW_TILE_LIT); then the list of
// tiles to soften.
#define VELK_SHADOW_WIDTH 0u       // the pixels a row
#define VELK_SHADOW_HEIGHT 1u      // the rows
#define VELK_SHADOW_ARGS 2u        // the softening's dispatch: its groups, at most 65535, 1 and 1
#define VELK_SHADOW_LISTED 5u      // the tiles listed
#define VELK_SHADOW_HEADER_WORDS 8u
#define VELK_SHADOW_LIGHTS 4u
#define VELK_SHADOW_TILE 8u
#define VELK_SHADOW_KEEP 255u
#define VELK_SHADOW_TILE_SHADOW 1u
#define VELK_SHADOW_TILE_LIT 2u
// The tiles around a tile a penumbra or the blocker search reaches into: kSunPenumbraPixels.
#define VELK_SHADOW_HALO 8

uint velk_shadow_pack_word(uint base, uint pixel)
{
    return base + VELK_SHADOW_HEADER_WORDS + pixel * VELK_SHADOW_LIGHTS;
}

uint velk_shadow_soft_word(uint base, uint pixels, uint pixel)
{
    return base + VELK_SHADOW_HEADER_WORDS + pixels * VELK_SHADOW_LIGHTS + pixel;
}

uint velk_shadow_tile_word(uint base, uint pixels, uint tile)
{
    return base + VELK_SHADOW_HEADER_WORDS + pixels * (VELK_SHADOW_LIGHTS + 1u) + tile;
}

uint velk_shadow_list_word(uint base, uint pixels, uint tiles)
{
    return velk_shadow_tile_word(base, pixels, tiles);
}

// A light the softening takes of a pixel: light `light`, `through` of it getting through as the
// points of light say, and its blocker code (velk_light_blocker_code), 0 where none is known. The
// light in the low 16 bits, what gets through in 255ths in the next 8, the code above.
uint velk_shadow_pack(uint light, float through, uint code)
{
    return (light & 0xFFFFu) | (uint(clamp(through, 0.0, 1.0) * 255.0 + 0.5) << 16u) | ((code & 15u) << 24u);
}

uint velk_shadow_light(uint packed) { return packed & 0xFFFFu; }
float velk_shadow_through(uint packed) { return float((packed >> 16u) & 255u) / 255.0; }
uint velk_shadow_code(uint packed) { return (packed >> 24u) & 15u; }
