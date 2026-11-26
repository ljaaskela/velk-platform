layout(set = 0, binding = 0) uniform usampler2D velk_textures_uint[];

#define RESOLVE_IDS pc.extras.x
#define RESOLVE_WIDTH pc.extras.y
#define RESOLVE_HEIGHT pc.extras.z
#define RESOLVE_ALBEDO pc.extras.w
#define RESOLVE_NORMAL pc.env.x
#define RESOLVE_POSITION pc.env.y
#define RESOLVE_MATERIAL pc.env.z
#define RESOLVE_EMISSIVE pc.env.w
#define RESOLVE_SPECULAR pc.more.x
// The resolve has no lights, so their count carries the word of the depth plane that compute raster drew to, a word a pixel
// (instance_tiny_draw_src). After it come the pixels' ids, two words a pixel
// (velk_pixel_ids): compute raster's where it drew, the hardware's written here elsewhere.
#define RESOLVE_TINY pc.light_count

layout(set = 1, binding = 16, std430) buffer ResolveWords { uint data[]; } resolve_words;

// Where the view ray through a point of the screen, in pixels, meets a triangle's plane. A
// ray rather than dividing by each corner's w, which fails for corners behind the camera. It
// starts on the near plane so that it also suits orthographic views. Leaves the results
// alone for a ray along the plane.
void resolve_hit(GlobalData g, vec2 pixel, vec3 p0, vec3 e1, vec3 e2, inout vec3 weights, inout vec3 point)
{
    vec2 ndc = pixel / vec2(float(RESOLVE_WIDTH), float(RESOLVE_HEIGHT)) * 2.0 - 1.0;
    vec4 near_point = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec4 far_point = g.inverse_view_projection * vec4(ndc, 1e-4, 1.0);
    vec3 origin = near_point.xyz / near_point.w;
    vec3 dir = normalize(far_point.xyz / far_point.w - origin);

    vec3 h = cross(dir, e2);
    float det = dot(e1, h);
    if (abs(det) > 0.0) {
        vec3 s = origin - p0;
        vec3 q = cross(s, e1);
        float b1 = dot(s, h) / det;
        float b2 = dot(dir, q) / det;
        weights = vec3(1.0 - b1 - b2, b1, b2);
        // Along the ray, so that imprecision in a large triangle's weights only moves
        // the point towards or away from the camera.
        point = origin + dir * (dot(e2, q) / det);
    }
}

vec3 resolve_shading_normal(MaterialEval e, EvalContext ctx)
{
    return normalize(length(e.normal) > 0.0 ? e.normal : ctx.normal);
}


#extension GL_KHR_shader_subgroup_quad : require

// A group's 64 threads as 16 blocks of 2x2 pixels, a subgroup quad each, so that a pixel
// reads its neighbours across and down with quad swaps.
ivec2 resolve_pixel()
{
    uint linear = gl_SubgroupID * gl_SubgroupSize + gl_SubgroupInvocationID;
    uint quad = linear >> 2u;
    uint lane = linear & 3u;
    return ivec2(gl_WorkGroupID.xy) * 8 + ivec2((quad & 3u) * 2u + (lane & 1u), (quad >> 2u) * 2u + (lane >> 1u));
}

void main()
{
    // Every thread goes on to the quad swaps, pixels off the image and empty ones too.
    ivec2 coord = resolve_pixel();
    bool inside = coord.x < int(RESOLVE_WIDTH) && coord.y < int(RESOLVE_HEIGHT);

    // Compute raster drew here, nearer than the hardware, and left its ids. Its depth is
    // cleared for the next frame.
    uvec2 ids = uvec2(0u);
    uint ids_at = 0u;
    if (inside) {
        uint pixel_index = uint(coord.y) * RESOLVE_WIDTH + uint(coord.x);
        ids_at = RESOLVE_TINY + RESOLVE_WIDTH * RESOLVE_HEIGHT + pixel_index * VELK_PIXEL_RECORD_WORDS;
        bool tiny = resolve_words.data[RESOLVE_TINY + pixel_index] != 0u;
        if (tiny) {
            resolve_words.data[RESOLVE_TINY + pixel_index] = 0u;
            ids = uvec2(resolve_words.data[ids_at], resolve_words.data[ids_at + 1u]);
        } else {
            ids = texelFetch(velk_textures_uint[nonuniformEXT(RESOLVE_IDS)], coord, 0).xy;
            resolve_words.data[ids_at] = ids.x;
            resolve_words.data[ids_at + 1u] = ids.y;
        }
        // The culling's trace (InstanceCullPass::trace), at its pixel: which raster's ids were
        // taken, and what they are.
        uint trace = VELK_GLOBALS.trace;
        if (trace != 0xFFFFFFFFu && uint(coord.x) == resolve_words.data[trace] && uint(coord.y) == resolve_words.data[trace + 1u]) {
            uint at = atomicAdd(resolve_words.data[trace + 2u], 1u);
            if (at < resolve_words.data[trace + 3u]) {
                uint w = trace + 6u + at * 4u;
                resolve_words.data[w] = 5u | ((tiny ? 1u : 0u) << 8u);
                resolve_words.data[w + 1u] = ids.x;
                resolve_words.data[w + 2u] = ids.y;
                resolve_words.data[w + 3u] = 0u;
            }
        }
    }
    bool drawn = ids.x != 0u;

    MaterialEval e = velk_default_material_eval();
    vec3 N = vec3(0.0);
    vec3 smooth_normal = vec3(0.0);
    vec3 rel = vec3(0.0);
    float lod_error = 0.0;
    float tri_size = 0.0;
    // A stand-in's flatness (velk_stand_in_flatness): a quad of a view's quads, or a quad standing
    // for small pieces; below 0 for a surface as drawn.
    float stand_in = -1.0;
    // The draw and instance, which a neighbour must share to count.
    uvec2 surface = uvec2(0xFFFFFFFFu);
    // Seen from both sides: only such a thin sheet passes light through (MaterialEval::transmission).
    bool two_sided = false;
    if (drawn) {
        VelkDrawData d = velk_draw_data.data[velk_ids_draw(ids)];
        two_sided = (d.flags & VELK_DRAW_DOUBLE_SIDED) != 0u;
        uint triangle = velk_ids_triangle(d, ids);
        uint slot = velk_ids_slot(d, ids);
        ElementInstance inst = velk_draw_instance(d, slot);
        GlobalData g = velk_globals.data[d.globals_base];
        surface = uvec2(velk_ids_draw(ids), slot);

        uint cluster = velk_ids_cluster(d, ids);
        uvec3 corners = velk_surface_corners(d, cluster, triangle);
        VelkSurfaceVertex v0 = velk_surface_vertex(d, cluster, inst, g, corners.x);
        VelkSurfaceVertex v1 = velk_surface_vertex(d, cluster, inst, g, corners.y);
        VelkSurfaceVertex v2 = velk_surface_vertex(d, cluster, inst, g, corners.z);

        vec3 e1 = v1.rel - v0.rel;
        vec3 e2 = v2.rel - v0.rel;
        vec2 pixel = vec2(coord) + 0.5;
        vec3 weights = vec3(1.0 / 3.0);
        rel = (v0.rel + v1.rel + v2.rel) / 3.0;
        resolve_hit(g, pixel, v0.rel, e1, e2, weights, rel);

        // For the passes after, which read the pixel's light: where it is in its triangle
        // (velk_pixel_weights), and the triangle's longest edge.
        vec3 b = clamp(weights, 0.0, 1.0);
        b /= max(b.x + b.y + b.z, 1e-6);
        resolve_words.data[ids_at + 2u] = floatBitsToUint(b.y);
        resolve_words.data[ids_at + 3u] = floatBitsToUint(b.z);
        vec3 e3 = v2.rel - v1.rel;
        tri_size = sqrt(max(max(dot(e1, e1), dot(e2, e2)), dot(e3, e3)));

        // Texture coordinates' steps to the next pixels along, on this triangle's plane even
        // where another covers them.
        vec3 weights_x = weights;
        vec3 weights_y = weights;
        vec3 rel_x = rel;
        vec3 rel_y = rel;
        resolve_hit(g, pixel + vec2(1.0, 0.0), v0.rel, e1, e2, weights_x, rel_x);
        resolve_hit(g, pixel + vec2(0.0, 1.0), v0.rel, e1, e2, weights_y, rel_y);
        mat3x2 uvs = mat3x2(v0.uv, v1.uv, v2.uv);
        mat3x2 uv1s = mat3x2(v0.uv1, v1.uv1, v2.uv1);
        velk_uv_dx[0] = uvs * (weights_x - weights);
        velk_uv_dy[0] = uvs * (weights_y - weights);
        velk_uv_dx[1] = uv1s * (weights_x - weights);
        velk_uv_dy[1] = uv1s * (weights_y - weights);

        EvalContext ctx = velk_surface_context(d, g, v0, v1, v2, weights, rel);
        velk_uv_set = 0u;
        e = velk_eval_material(d.material_id, ctx);
        N = resolve_shading_normal(e, ctx);
        smooth_normal = ctx.normal;
        // Seen from both sides: the side it shows is the one toward the camera, of its plane as
        // drawn (whatever its winding), and its normal turns to that side. Which side its normals
        // are on is its vertex normal's, so that a normal map cannot tip it.
        if (two_sided) {
            vec3 plane = cross(e1, e2);
            if (dot(smooth_normal, plane) * dot(-rel, plane) < 0.0) {
                N = -N;
                smooth_normal = -smooth_normal;
            }
        }
        // A quad of a view's quads by its node's surface, else by its normals' length, which only a
        // level of detail's data (a cluster's) keeps for it.
        if ((d.flags & VELK_DRAW_QUADS) != 0u) {
            stand_in = velk_surface_flatness(inst.params.y);
            e.roughness = velk_surface_roughness(inst.params.y);
        } else if (cluster != VELK_NO_CLUSTER) {
            stand_in = velk_stand_in_flatness(dot(b, vec3(v0.normal_length, v1.normal_length, v2.normal_length)));
        }

        mat3 world_axes = mat3(inst.world_matrix);
        lod_error = velk_cluster_error(cluster, max(max(length(world_axes[0]), length(world_axes[1])), length(world_axes[2])));
    }

    // Widens roughness where the shading normal changes quickly between pixels, so that a
    // tight highlight does not alias (Tokuyoshi 2019). A neighbour counts on the same draw and
    // instance with a smooth normal close to this pixel's, so that an edge between surfaces
    // does not; without one across or down, the other stands in for it.
    vec3 n_x = subgroupQuadSwapHorizontal(N);
    vec3 n_y = subgroupQuadSwapVertical(N);
    vec3 smooth_x = subgroupQuadSwapHorizontal(smooth_normal);
    vec3 smooth_y = subgroupQuadSwapVertical(smooth_normal);
    uvec2 surface_x = subgroupQuadSwapHorizontal(surface);
    uvec2 surface_y = subgroupQuadSwapVertical(surface);
    if (!drawn) {
        if (inside) {
            imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, vec4(0.0));
            imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(0.0));
            imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(0.0));
            imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord, vec4(0.0));
            imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(0.0));
            imageStore(gStorageImages[nonuniformEXT(RESOLVE_SPECULAR)], coord, vec4(0.0));
        }
        return;
    }
    bool across = surface_x == surface && dot(smooth_x, smooth_normal) > 0.9;
    bool down = surface_y == surface && dot(smooth_y, smooth_normal) > 0.9;
    vec3 dndx = across ? n_x - N : vec3(0.0);
    vec3 dndy = down ? n_y - N : vec3(0.0);
    if (!across) dndx = dndy;
    if (!down) dndy = dndx;
    float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
    float roughness = sqrt(clamp(e.roughness * e.roughness + min(2.0 * variance, 0.18), 0.0, 1.0));

    imageStore(gStorageImages[nonuniformEXT(RESOLVE_ALBEDO)], coord, e.color);
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_NORMAL)], coord, vec4(N, lod_error));
    imageStore(gStorageImagesF32[nonuniformEXT(RESOLVE_POSITION)], coord, vec4(rel, tri_size));
    imageStore(gStorageImages[nonuniformEXT(RESOLVE_MATERIAL)], coord,
               vec4(e.metallic, roughness, float(e.lighting_mode) / 255.0, velk_stand_in_pack(stand_in)));
    imageStore(gStorageImagesF16[nonuniformEXT(RESOLVE_EMISSIVE)], coord, vec4(e.emissive, two_sided ? clamp(e.transmission, 0.0, 1.0) : 0.0));
    // As a dielectric (KHR_materials_specular, velk_specular_f0): the square root of its
    // reflectance at normal incidence, for steps fine near 0.04 and room up to 1, and its
    // reflectance at grazing.
    float specular = clamp(e.specular_factor, 0.0, 1.0);
    vec3 dielectric_f0 = min(0.04 * max(e.specular_color_factor, vec3(0.0)), vec3(1.0)) * specular;
    imageStore(gStorageImages[nonuniformEXT(RESOLVE_SPECULAR)], coord, vec4(sqrt(dielectric_f0), specular));
}
