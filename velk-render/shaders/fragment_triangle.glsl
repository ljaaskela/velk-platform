// For the fragment at `rel_pos` with ids `ids` (as VELK_WRITE_VISIBILITY writes them) of draw
// `d`: where it is in its triangle, as weights of its corners, and its triangle's longest edge,
// in world units. A third each and no edge when it is not of a cluster.
void velk_fragment_triangle(VelkDrawData d, uvec2 ids, vec3 rel_pos, out vec3 weights, out float longest)
{
    weights = vec3(1.0 / 3.0);
    longest = 0.0;
    uint cluster = velk_ids_cluster(d, ids);
    if (cluster == VELK_NO_CLUSTER) return;
    uint word = d.index_base + velk_cluster_first_index(cluster) + velk_ids_triangle(d, ids) * 3u;
    uint base = velk_cluster_vertex_word(d, cluster);
    ElementInstance inst = velk_draw_instance(d, velk_ids_slot(d, ids));
    GlobalData g = velk_globals.data[d.globals_base];
    vec3 p0 = velk_rel_position(inst.world_matrix, inst.offset.xyz
        + velk_unpack_vertex3d(base + velk_mesh_words.data[word] * VELK_VERTEX3D_WORDS).position * inst.size.xyz, g);
    vec3 p1 = velk_rel_position(inst.world_matrix, inst.offset.xyz
        + velk_unpack_vertex3d(base + velk_mesh_words.data[word + 1u] * VELK_VERTEX3D_WORDS).position * inst.size.xyz, g);
    vec3 p2 = velk_rel_position(inst.world_matrix, inst.offset.xyz
        + velk_unpack_vertex3d(base + velk_mesh_words.data[word + 2u] * VELK_VERTEX3D_WORDS).position * inst.size.xyz, g);
    vec3 e0 = p1 - p0;
    vec3 e1 = p2 - p0;
    vec3 e2 = rel_pos - p0;
    float d00 = dot(e0, e0);
    float d01 = dot(e0, e1);
    float d11 = dot(e1, e1);
    float d20 = dot(e2, e0);
    float d21 = dot(e2, e1);
    float denom = d00 * d11 - d01 * d01;
    if (denom > 0.0) {
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        vec3 b = max(vec3(1.0 - v - w, v, w), vec3(0.0));
        weights = b / max(b.x + b.y + b.z, 1e-6);
    }
    longest = sqrt(max(max(d00, d11), dot(p2 - p1, p2 - p1)));
}
