#ifdef VELK_LOD_LIGHT
vec3 g_view_cam;
bool g_out_of_view;
uint g_visited;  // hierarchy nodes this thread walked

// Whether a sphere at `center` (world space) of `radius` is out of the camera's view, widened
// by a quarter on each side, so that turning a little keeps what was chosen.
bool velk_lod_out_of_view(vec3 center, float radius, mat4 vp)
{
    vec4 clip = vp * vec4(center - g_view_cam, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w = clip.w * 1.25 + radius * length(r3);
    return clip.w + radius * length(r3) < 0.0 || abs(clip.x) > w + radius * length(r0) || abs(clip.y) > w + radius * length(r1);
}
#endif
vec3 velk_lod_center(uint word, mat4 world, vec3 cam)
{
    return mat3(world) * vec3(uintBitsToFloat(cull_words.data[word]), uintBitsToFloat(cull_words.data[word + 1u]),
                              uintBitsToFloat(cull_words.data[word + 2u]))
        + (world[3].xyz - cam);
}

// `word` holds a sphere's center and radius, then an error.
float velk_lod_pixels(uint word, mat4 world, float scale, vec3 cam, float pixels_per_unit)
{
    float away = length(velk_lod_center(word, world, cam)) - uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return uintBitsToFloat(cull_words.data[word + 4u]) * scale * pixels_per_unit / max(away, 1e-3);
}

// What child `child` of a hierarchy node is to the choice (VELK_LOD_SKIP, VELK_LOD_DESCEND,
// VELK_LOD_CHOSEN or VELK_LOD_FOR_RAYS), and in `c` the node it is or its cluster.
// `clusters` is the entry's cluster words.
#define VELK_LOD_SKIP 0u
#define VELK_LOD_DESCEND 1u
#define VELK_LOD_CHOSEN 2u
#define VELK_LOD_FOR_RAYS 3u
uint velk_lod_child(uint child, uint clusters, mat4 world, float scale, vec3 lod_cam, mat4 vp, float pixels_per_unit,
                    float max_pixels, out uint c)
{
    c = 0u;
    float per_error = scale * pixels_per_unit;
#ifdef VELK_LOD_LIGHT
    float coarsest = max(max_pixels, pc.light_pixels);
#else
    float coarsest = max_pixels;
#endif
    // A node's child is 8 words: a sphere holding the spheres the clusters under it are tested
    // with, the smallest of their errors, the largest of their parents', and what it is.
    uint ref = lod_nodes.data[child + 6u];
    if (ref == VELK_LOD_NO_CHILD) return VELK_LOD_SKIP;
    // Skipped when all under it are too coarse, or all have a parent that will do.
    vec3 center = vec3(uintBitsToFloat(lod_nodes.data[child]), uintBitsToFloat(lod_nodes.data[child + 1u]),
                       uintBitsToFloat(lod_nodes.data[child + 2u]));
    float dist = length(mat3(world) * center + (world[3].xyz - lod_cam));
    float reach = uintBitsToFloat(lod_nodes.data[child + 3u]) * scale;
    float finest = max_pixels;
#ifdef VELK_LOD_LIGHT
    // Out of view: no finer than the light's level.
    if (velk_lod_out_of_view(mat3(world) * center + world[3].xyz, reach, vp)) finest = pc.light_pixels;
#endif
#ifdef VELK_TRACE_WALK
    // The tree's shape (kTraceShape): which node this child is under, and which of its two.
    velk_trace_shape(ref, child);
    // The node's two bounds, as stored and on screen, for every child the walk of the entry
    // traced tests.
    velk_trace_numbers(ref | 0x08000000u, uintBitsToFloat(lod_nodes.data[child + 4u]),
                       uintBitsToFloat(lod_nodes.data[child + 4u]) * per_error / (dist + reach));
    velk_trace_numbers(ref | 0x04000000u, uintBitsToFloat(lod_nodes.data[child + 5u]),
                       uintBitsToFloat(lod_nodes.data[child + 5u]) * per_error / max(dist - reach, 1e-3));
#endif
    if (uintBitsToFloat(lod_nodes.data[child + 4u]) * per_error / (dist + reach) > coarsest) {
#ifdef VELK_TRACE_WALK
        if ((ref & VELK_LOD_LEAF) != 0u) velk_trace_walk(clusters, ref & ~VELK_LOD_LEAF, world, scale, 4u);
        else velk_trace_node(mat3(world) * center + world[3].xyz, reach, ref, 8u);
#endif
        return VELK_LOD_SKIP;
    }
    if (uintBitsToFloat(lod_nodes.data[child + 5u]) * per_error / max(dist - reach, 1e-3) <= finest) {
#ifdef VELK_TRACE_WALK
        if ((ref & VELK_LOD_LEAF) != 0u) velk_trace_walk(clusters, ref & ~VELK_LOD_LEAF, world, scale, 5u);
        else velk_trace_node(mat3(world) * center + world[3].xyz, reach, ref, finest > max_pixels ? 10u : 9u);
#endif
        return VELK_LOD_SKIP;
    }
    if ((ref & VELK_LOD_LEAF) == 0u) {
        c = ref;
        return VELK_LOD_DESCEND;
    }

    c = ref & ~VELK_LOD_LEAF;
    uint cluster = clusters + c * VELK_LOD_CLUSTER_WORDS;
    float own = velk_lod_pixels(cluster + VELK_LOD_CLUSTER_SPHERE, world, scale, lod_cam, pixels_per_unit);
    float parent = velk_lod_pixels(cluster + VELK_LOD_CLUSTER_PARENT_SPHERE, world, scale, lod_cam, pixels_per_unit);
    float limit = max_pixels;
#ifdef VELK_LOD_LIGHT
    // Out of view, the light's level is all there is of it: rays and light use it, and it
    // is drawn if it comes into view before its partition chooses again.
    vec3 at = mat3(world) * vec3(uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE]),
                                 uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 1u]),
                                 uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 2u])) + world[3].xyz;
    bool out_of_view = velk_lod_out_of_view(at, uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 3u]) * scale, vp);
    if (out_of_view) {
        limit = pc.light_pixels;
        g_out_of_view = true;
    }
#endif
#ifdef VELK_TRACE_WALK
    // The cluster's own error and its parent's, as stored and on screen.
    velk_trace_numbers(c | 0x02000000u, uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_SPHERE + 4u]), own);
    velk_trace_numbers(c | 0x01000000u, uintBitsToFloat(cull_words.data[cluster + VELK_LOD_CLUSTER_PARENT_SPHERE + 4u]), parent);
    // Traced: chosen (2), for rays only (3), too coarse (6), or its parent will do (7).
    velk_trace_walk(clusters, c, world, scale,
                    own <= limit && parent > limit ? 2u
#ifdef VELK_LOD_LIGHT
                    : !out_of_view && own <= pc.light_pixels && parent > pc.light_pixels ? 3u
#endif
                    : own > limit ? 6u : 7u);
#endif
    if (own <= limit && parent > limit) return VELK_LOD_CHOSEN;
#ifdef VELK_LOD_LIGHT
    if (!out_of_view && own <= pc.light_pixels && parent > pc.light_pixels) return VELK_LOD_FOR_RAYS;
#endif
    return VELK_LOD_SKIP;
}

// The walk's stack: 32 for each thread of the work group, which has 64, in shared memory, since as
// an array of its own it would be in scratch memory.
shared uint s_lod_stack[64u * 32u];

// Walks the tree of `entry` from node `start` down (0 is its root), depth first. `cam` is the
// camera, which occlusion and compute raster go by; `lod_cam` where levels of detail are chosen
// from: the camera, or its partition's viewpoint.
void velk_lod_walk(uint entry, uint start, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, mat4 vp, float height,
                   float max_pixels)
{
    float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * height * 0.5;
    uint clusters = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE];
    uint nodes = cull_words.data[entry + VELK_CLUSTER_ENTRY_NODES];
    // A deforming mesh's spheres are where its clusters are at rest, which says nothing
    // about whether they are hidden.
    bool deforming = (cull_words.data[entry + VELK_CLUSTER_ENTRY_FLAGS] & VELK_LOD_DEFORMING) != 0u;
    // The tree's nodes are among the mesh's nodes that rays walk, in lod_nodes. The tree is no
    // deeper than kLodMaxDepth.
    uint stack = gl_LocalInvocationIndex * 32u;
    int sp = 0;
    s_lod_stack[stack + uint(sp++)] = start;
    while (sp > 0) {
        uint node = nodes + s_lod_stack[stack + uint(--sp)] * VELK_LOD_NODE_STRIDE;
#ifdef VELK_LOD_LIGHT
        ++g_visited;
#endif
        // The second child first on the stack, so that the first child's subtree is walked
        // first: the order velk_lod_child's callers expand nodes in.
        uint later = VELK_LOD_NO_CHILD;
        for (uint k = 0u; k < 2u; ++k) {
            uint c;
            uint what = velk_lod_child(node + k * 8u, clusters, world, scale, lod_cam, vp, pixels_per_unit, max_pixels, c);
            if (what == VELK_LOD_DESCEND) {
                if (k == 0u) {
                    later = c;
                } else if (sp < 32) {
                    s_lod_stack[stack + uint(sp++)] = c;
                }
            } else if (what == VELK_LOD_CHOSEN) {
                velk_lod_chosen(entry, c, slot, world, scale, cam, lod_cam, pixels_per_unit, deforming);
            }
#ifdef VELK_LOD_LIGHT
            else if (what == VELK_LOD_FOR_RAYS) {
                velk_lod_light_chosen(entry, c, slot);
            }
#endif
        }
        if (later != VELK_LOD_NO_CHILD && sp < 32) s_lod_stack[stack + uint(sp++)] = later;
    }
}

void velk_lod_visit(uint entry, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, mat4 vp, float height,
                    float max_pixels)
{
    velk_lod_walk(entry, 0u, slot, world, scale, cam, lod_cam, vp, height, max_pixels);
}
