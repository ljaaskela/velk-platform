#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;
layout(set = 1, binding = 12, std430) readonly buffer LodNodes { uint data[]; } lod_nodes;

layout(push_constant) uniform PC {
    uint retest_word;
    uint retest_capacity;
    uint globals_base;
    uint pyramid_word;
    float lod_max_pixels;
    uint tiny_word;
    uint tiny_capacity;
    float tiny_pixels;
} pc;
#velk_include VELK_OCCLUSION_GLSL
#velk_include VELK_TINY_ADD_GLSL
uint g_chosen_word;
uint g_chosen_capacity;

void velk_lod_use(uint entry, uint cluster, uint slot, uint cut_record)
{
    uint at = atomicAdd(cull_words.data[g_chosen_word + VELK_CHOSEN_LIST_COUNT], 1u);
    if (at >= g_chosen_capacity) return;
    uint record = g_chosen_word + VELK_CHOSEN_LIST_RECORDS + at * VELK_CHOSEN_END;
    cull_words.data[record + VELK_CHOSEN_ENTRY] = entry;
    cull_words.data[record + VELK_CHOSEN_CLUSTER] = cluster;
    cull_words.data[record + VELK_CHOSEN_SLOT] = slot;
    cull_words.data[record + VELK_CHOSEN_CUT_RECORD] = cut_record;
    atomicAdd(cull_words.data[cull_words.data[entry + VELK_CLUSTER_ENTRY_COUNTS] + cluster], 1u);
}

bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam);
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming);
#velk_include VELK_LOD_SELECT_GLSL
#velk_include VELK_LOD_TAKE_GLSL
bool velk_lod_occluded(uint word, mat4 world, float scale, vec3 cam)
{
    GlobalData g = velk_globals.data[pc.globals_base];
    float radius = uintBitsToFloat(cull_words.data[word + 3u]) * scale;
    return velk_behind_pyramid(pc.pyramid_word, g.view_projection, velk_lod_center(word, world, cam), radius);
}

// Of a whole instance chosen every frame, so from no kept cut record.
void velk_lod_chosen(uint entry, uint c, uint slot, mat4 world, float scale, vec3 cam, vec3 lod_cam, float pixels_per_unit,
                     bool deforming)
{
    uint cluster = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + c * VELK_LOD_CLUSTER_WORDS;
    if (deforming || !velk_lod_occluded(cluster + VELK_LOD_CLUSTER_SPHERE, world, scale, cam)) {
        velk_lod_take(entry, c, slot, 0xFFFFFFFFu, world, scale, cam, lod_cam, pixels_per_unit);
    }
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= min(cull_words.data[pc.retest_word + VELK_RETEST_LIST_COUNT], pc.retest_capacity)) return;

    uint record = pc.retest_word + VELK_RETEST_LIST_RECORDS + index * VELK_RETEST_END;
    uint instance = cull_words.data[record + VELK_RETEST_INSTANCE];
    uint slot = cull_words.data[record + VELK_RETEST_SLOT];
    uint table_word = cull_words.data[record + VELK_RETEST_TABLE];
    uint entry = cull_words.data[record + VELK_RETEST_ENTRY];
    g_chosen_word = cull_words.data[record + VELK_RETEST_CHOSEN_WORD];
    g_chosen_capacity = cull_words.data[record + VELK_RETEST_CHOSEN_CAPACITY];

    CullInstance ci = cull_instances.data[instance];
    mat4 world = velk_sized_world(ci.world_matrix, ci.offset.xyz, ci.size.xyz);
    float scale = sqrt(max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                           dot(world[2].xyz, world[2].xyz)));
    GlobalData globals = velk_globals.data[pc.globals_base];
    // Its levels of detail are chosen from the camera, as the first test chose them.
    vec3 cam = globals.cam_pos.xyz;
    vec3 lod_cam = cam;

    if (entry != 0xFFFFFFFFu) {
        uint cluster = cull_words.data[record + VELK_RETEST_CLUSTER];
        uint sphere = cull_words.data[entry + VELK_CLUSTER_ENTRY_TABLE] + cluster * VELK_LOD_CLUSTER_WORDS + VELK_LOD_CLUSTER_SPHERE;
        if (!velk_lod_occluded(sphere, world, scale, cam)) {
            mat4 vp = globals.view_projection;
            float pixels_per_unit = length(vec3(vp[0][1], vp[1][1], vp[2][1])) * globals.viewport.y * 0.5;
            velk_lod_take(entry, cluster, slot, cull_words.data[record + VELK_RETEST_CUT_RECORD], world, scale, cam, lod_cam,
                          pixels_per_unit);
        }
        return;
    }

    uint table = table_word + cull_instances.data[instance].params.y * VELK_TABLE_END;
    vec3 local_center = vec3(uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER]),
                             uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 1u]),
                             uintBitsToFloat(cull_words.data[table + VELK_TABLE_CENTER + 2u]));
    float radius = uintBitsToFloat(cull_words.data[table + VELK_TABLE_RADIUS]) * scale;
    if (velk_behind_pyramid(pc.pyramid_word, globals.view_projection, velk_rel_position(world, local_center, globals), radius)) {
        return;
    }

    // After the positions the first test filled. A table without a list has no capacity.
    uint position = cull_words.data[table + VELK_TABLE_COUNT] + atomicAdd(cull_words.data[table + VELK_TABLE_SECOND_COUNT], 1u);
    uint capacity = cull_words.data[table + VELK_TABLE_CAPACITY];
    if (position < capacity) {
        uint list = cull_words.data[table + VELK_TABLE_LIST];
        cull_words.data[list + position] = slot;
        cull_words.data[list + capacity + position] = 0xFFFFFFFFu;
    } else {
        // No room: the count, which the second draw's instance count is, gives it back, so that
        // it never reads past the list.
        atomicAdd(cull_words.data[table + VELK_TABLE_SECOND_COUNT], 0xFFFFFFFFu);
    }

    uint first_entry = cull_words.data[table + VELK_TABLE_FIRST_ENTRY];
    uint entries = cull_words.data[table + VELK_TABLE_ENTRY_COUNT];
    for (uint e = 0u; e < entries; ++e) {
        velk_lod_visit(first_entry + e * VELK_CLUSTER_ENTRY_WORDS, slot, world, scale, cam, lod_cam, globals.view_projection,
                       globals.viewport.y, pc.lod_max_pixels);
    }
}
