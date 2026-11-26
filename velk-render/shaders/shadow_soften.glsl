#define VELK_LIGHT_WORDS velk_light_cell_words
#velk_include VELK_LIGHT_POOL_GLSL
#velk_include VELK_SHADOW_SOFT_GLSL
// The soft shadows' third pass, a group a listed tile, a thread a pixel: each light the first pass
// took of the pixel spread over the penumbra its size gives, from what the points of light say,
// with no rays. First how far
// away what keeps it is, the mean of the pixel's and of the pixels around within
// kSunSearchPixels that took the same light and know it; then what of it gets through at the
// pixels within the penumbra that took the same light. Both take only pixels on a surface like
// the pixel's. A light whose search finds nothing, or whose penumbra is under half a pixel, keeps
// what the points say (VELK_SHADOW_KEEP). pc.spare0 is the soft shadows' first word.

// The penumbra's half axes `ua` and `ub` in the world, for light `l` seen from `p` along `L`, with
// what keeps it `reach` away (velk_light_blocker_reach): a directional light's disc, as wide as
// its angle over that distance; a light at a place, its disc or its square as large as it looks
// from `p` through what keeps it.
void velk_shadow_kernel(Light l, vec3 p, float reach, out vec3 L, out vec3 ua, out vec3 ub)
{
    float r;
    if (l.flags.x == 0u) {
        L = normalize(-l.direction.xyz);
        r = tan(max(l.params.w, 0.0)) * reach;
    } else {
        vec3 to = l.position.xyz - p;
        L = to / max(length(to), 1e-6);
        float f = clamp(reach, 0.0, 0.95);
        r = l.params.w * f / (1.0 - f);
        if (l.flags.x == 3u) {
            velk_area_axes(l.direction.xyz, l.params.w, ua, ub);
            ua *= f / (1.0 - f);
            ub *= f / (1.0 - f);
            return;
        }
    }
    vec3 a = normalize(abs(L.y) < 0.9 ? cross(L, vec3(0.0, 1.0, 0.0)) : cross(L, vec3(1.0, 0.0, 0.0)));
    ua = a * r;
    ub = cross(L, a) * r;
}

// Of the pixel `pixel`'s lights the first pass took, the one that is light `light`:
// VELK_LIGHT_NONE for none.
uint velk_shadow_find(uint base, uint pixel, uint light)
{
    uint at = velk_shadow_pack_word(base, pixel);
    uint found = VELK_LIGHT_NONE;
    for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
        uint packed = velk_light_cell_words.data[at + j];
        if (packed != VELK_LIGHT_NONE && velk_shadow_light(packed) == light) found = packed;
    }
    return found;
}

void main()
{
    uint base = pc.spare0;
    uint width = velk_light_cell_words.data[base + VELK_SHADOW_WIDTH];
    uint height = velk_light_cell_words.data[base + VELK_SHADOW_HEIGHT];
    uint pixels = width * height;
    uint tiles_x = (width + VELK_SHADOW_TILE - 1u) / VELK_SHADOW_TILE;
    uint tiles = tiles_x * ((height + VELK_SHADOW_TILE - 1u) / VELK_SHADOW_TILE);
    uint listed = min(velk_light_cell_words.data[base + VELK_SHADOW_LISTED], tiles);
    // Past 65535 tiles, a group takes every 65535th.
    for (uint n = gl_WorkGroupID.x; n < listed; n += 65535u) {
        uint tile = velk_light_cell_words.data[velk_shadow_list_word(base, pixels, tiles) + n];
        ivec2 coord = ivec2(int(tile % tiles_x), int(tile / tiles_x)) * int(VELK_SHADOW_TILE) + ivec2(gl_LocalInvocationID.xy);
        if (uint(coord.x) >= width || uint(coord.y) >= height) continue;
        uint pixel = uint(coord.y) * width + uint(coord.x);
        uint own[VELK_SHADOW_LIGHTS];
        bool any = false;
        uint at = velk_shadow_pack_word(base, pixel);
        for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
            own[j] = velk_light_cell_words.data[at + j];
            any = any || own[j] != VELK_LIGHT_NONE;
        }
        if (!any) continue;
        vec3 n3 = texelFetch(velk_textures[nonuniformEXT(pc.normal_tex_id)], coord, 0).xyz;
        vec3 p = texelFetch(velk_textures[nonuniformEXT(pc.worldpos_tex_id)], coord, 0).xyz;
        mat4 vp = VELK_GLOBALS.view_projection;
        float clip_w = (vp * vec4(p, 1.0)).w;
        if (dot(n3, n3) < 1e-6 || !(clip_w > 1e-6)) continue;
        vec3 nrm = normalize(n3);
        // Pixels a world unit spans at this depth.
        float per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * VELK_GLOBALS.viewport.y * 0.5 / clip_w;
        uint h = (uint(coord.x) * 0x8da6b343u) ^ (uint(coord.y) * 0xd8163841u);
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
        float turn = float(h & 0xFFFFu) / 65536.0 * 6.2831853;
        bool stand_in = velk_stand_in_unpack(texelFetch(velk_textures[nonuniformEXT(pc.material_tex_id)], coord, 0).a) >= 0.0;

        // How far away what keeps each light is: the reach of the pixel's own code and of the
        // pixels around that took the same light and know theirs, on a surface like this one.
        float sum[VELK_SHADOW_LIGHTS];
        float count[VELK_SHADOW_LIGHTS];
        bool directional[VELK_SHADOW_LIGHTS];
        for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
            sum[j] = 0.0;
            count[j] = 0.0;
            directional[j] = false;
            if (own[j] == VELK_LIGHT_NONE) continue;
            directional[j] = velk_light_at(pc.lights_base + velk_shadow_light(own[j])).flags.x == 0u;
            if (velk_shadow_code(own[j]) == 0u) continue;
            sum[j] = velk_light_blocker_reach(velk_shadow_code(own[j]), directional[j]);
            count[j] = 1.0;
        }
        for (uint k = 0u; k < kSunTaps; ++k) {
            ivec2 q = velk_sun_tap(coord, k, kSunSearchPixels, turn);
            if (q.x < 0 || q.y < 0 || uint(q.x) >= width || uint(q.y) >= height) continue;
            uint qat = velk_shadow_pack_word(base, uint(q.y) * width + uint(q.x));
            uint theirs[VELK_SHADOW_LIGHTS];
            bool known = false;
            for (uint i = 0u; i < VELK_SHADOW_LIGHTS; ++i) {
                theirs[i] = velk_light_cell_words.data[qat + i];
                known = known || (theirs[i] != VELK_LIGHT_NONE && velk_shadow_code(theirs[i]) != 0u);
            }
            if (!known) continue;
            vec3 pq;
            if (!velk_sun_alike(q, p, nrm, pq)) continue;
            for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
                if (own[j] == VELK_LIGHT_NONE) continue;
                for (uint i = 0u; i < VELK_SHADOW_LIGHTS; ++i) {
                    if (theirs[i] == VELK_LIGHT_NONE || velk_shadow_light(theirs[i]) != velk_shadow_light(own[j])) continue;
                    uint code = velk_shadow_code(theirs[i]);
                    if (code == 0u) continue;
                    sum[j] += velk_light_blocker_reach(code, directional[j]);
                    count[j] += 1.0;
                }
            }
        }

        // Each light over its penumbra on the receiver: the kernel carried along the light onto
        // the pixel's plane and projected to the screen, a disc in the screen for a stand-in or a
        // plane nearly along the light, scaled down as a whole past kSunPenumbraPixels; what of
        // it gets through at the pixels within that took the same light, on a surface like this.
        uint result = 0u;
        for (uint j = 0u; j < VELK_SHADOW_LIGHTS; ++j) {
            uint keep = VELK_SHADOW_KEEP << (8u * j);
            if (own[j] == VELK_LIGHT_NONE || count[j] == 0.0) {
                result |= keep;
                continue;
            }
            uint light = velk_shadow_light(own[j]);
            Light l = velk_light_at(pc.lights_base + light);
            vec3 L;
            vec3 ua;
            vec3 ub;
            velk_shadow_kernel(l, p, sum[j] / count[j], L, ua, ub);
            float lean = dot(nrm, L);
            bool on_screen = stand_in || abs(lean) < 0.15;
            vec2 centre = vec2(coord) + 0.5;
            vec2 offsets[kSunTaps];
            uint landed = 0u;
            float wide = 0.0;
            for (uint k = 0u; k < kSunTaps; ++k) {
                float a = float(k) * 2.39996323 + turn + 1.3;
                float r = sqrt((float(k) + 0.5) / float(kSunTaps));
                vec3 u = (ua * cos(a) + ub * sin(a)) * r;
                offsets[k] = vec2(0.0);
                if (on_screen) {
                    offsets[k] = vec2(cos(a), sin(a)) * r * max(length(ua), length(ub)) * per_unit;
                } else {
                    vec2 px;
                    if (!velk_sun_land(p, nrm, L, lean, u, vp, px)) continue;
                    offsets[k] = px - centre;
                }
                landed |= 1u << k;
                wide = max(wide, length(offsets[k]));
            }
            float scale = wide > kSunPenumbraPixels ? kSunPenumbraPixels / wide : 1.0;
            if (wide * scale < 0.5) {
                result |= keep;
                continue;
            }
            float seen = velk_shadow_through(own[j]);
            float weight = 1.0;
            for (uint k = 0u; k < kSunTaps; ++k) {
                if ((landed & (1u << k)) == 0u) continue;
                ivec2 q = ivec2(floor(centre + offsets[k] * scale));
                if (q.x < 0 || q.y < 0 || uint(q.x) >= width || uint(q.y) >= height) continue;
                uint theirs = velk_shadow_find(base, uint(q.y) * width + uint(q.x), light);
                if (theirs == VELK_LIGHT_NONE) continue;
                vec3 pq;
                if (!velk_sun_alike(q, p, nrm, pq)) continue;
                seen += velk_shadow_through(theirs);
                weight += 1.0;
            }
            result |= min(uint(seen / weight * 254.0 + 0.5), 254u) << (8u * j);
        }
        velk_light_cell_words.data[velk_shadow_soft_word(base, pixels, pixel)] = result;
    }
}
