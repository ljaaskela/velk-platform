// A corner of a drawn triangle, as its material's vertex function gives it.
struct VelkSurfaceVertex {
    vec3 rel;       // in world space, from the camera
    vec3 normal;
    vec2 uv;
    vec2 uv1;
    vec4 tangent;
    vec4 color;
    uint shape_param;
    float normal_length;  // of its normal as authored, before the vertex function (velk_stand_in_flatness)
};

VelkSurfaceVertex velk_surface_vertex(VelkDrawData d, uint cluster, ElementInstance inst, GlobalData g, uint index)
{
    VelkVertex3D v = velk_unpack_vertex3d(velk_cluster_vertex_word(d, cluster) + index * VELK_VERTEX3D_WORDS);
    VelkVaryings s = velk_material_varyings(d.material_id, v, inst, g);
    VelkSurfaceVertex r;
    r.normal_length = length(v.normal);
    r.rel = s.rel_pos;
    r.normal = s.world_normal;
    r.uv = s.local_uv;
    r.uv1 = velk_read_uv1(velk_cluster_uv1_word(d, cluster, index));
    r.tangent = s.world_tangent;
    r.color = s.color * velk_read_color(velk_cluster_color_word(d, cluster, index));
    r.shape_param = s.shape_param;
    return r;
}

// The vertex indices of a triangle of a draw's cluster. A triangle strip's triangles start
// one vertex apart.
uvec3 velk_surface_corners(VelkDrawData d, uint cluster, uint triangle)
{
    if (d.index_base == VELK_NO_VISIBLE_LIST) return uvec3(triangle) + uvec3(0u, 1u, 2u);
    uint word = d.index_base + velk_cluster_first_index(cluster) + triangle * 3u;
    return uvec3(velk_mesh_words.data[word], velk_mesh_words.data[word + 1u], velk_mesh_words.data[word + 2u]);
}

// What the material is given at the point with corner weights @p weights, at @p rel from the
// camera of @p g.
EvalContext velk_surface_context(VelkDrawData d, GlobalData g, VelkSurfaceVertex v0, VelkSurfaceVertex v1,
                                 VelkSurfaceVertex v2, vec3 weights, vec3 rel)
{
    EvalContext ctx;
    ctx.material_base = d.material_words;
    ctx.texture_id = d.texture_id;
    ctx.shape_param = v0.shape_param;
    ctx.uv = v0.uv * weights.x + v1.uv * weights.y + v2.uv * weights.z;
    ctx.uv1 = v0.uv1 * weights.x + v1.uv1 * weights.y + v2.uv1 * weights.z;
    ctx.base = v0.color * weights.x + v1.color * weights.y + v2.color * weights.z;
    ctx.ray_dir = normalize(rel);
    ctx.normal = v0.normal * weights.x + v1.normal * weights.y + v2.normal * weights.z;
    ctx.hit_pos = rel + g.cam_pos.xyz;
    ctx.rel_pos = rel;
    ctx.tangent = v0.tangent * weights.x + v1.tangent * weights.y + v2.tangent * weights.z;
    return ctx;
}

// What the material of what a ray hit is given, as a pixel's is: from the hit's draw,
// instance, cluster and corners (RayHit, velk_hit_from_words), with the ray's direction,
// where it hit from the camera, and the surface's normal there for an analytic shape.
// `material` is the id velk_eval_material takes, 0 when the hit names no draw.
EvalContext velk_hit_context(RayHit hit, vec3 ray_dir, vec3 rel, vec3 normal, out uint material)
{
    EvalContext ctx;
    ctx.material_base = 0u;
    ctx.texture_id = 0u;
    ctx.shape_param = 0u;
    ctx.uv = hit.uv;
    ctx.uv1 = hit.uv;
    ctx.base = vec4(1.0);
    ctx.normal = normal;
    ctx.tangent = vec4(0.0);
    material = 0u;
    velk_hit_two_sided = false;
    if (hit.draw != 0xFFFFFFFFu && hit.instance != 0xFFFFFFFFu) {
        VelkDrawData d = velk_draw_data.data[hit.draw];
        velk_hit_two_sided = (d.flags & VELK_DRAW_DOUBLE_SIDED) != 0u;
        // A quad's hit names the quad's word, as its pixel does.
        ElementInstance inst = (d.flags & VELK_DRAW_QUADS) != 0u ? velk_quad_instance(hit.instance) : velk_instances.data[hit.instance];
        material = d.material_id;
        if (hit.corners.x == VELK_NO_CORNERS) {
            ctx.material_base = d.material_words;
            ctx.texture_id = d.texture_id;
            ctx.shape_param = inst.params.x;
            ctx.base = inst.color;
        } else {
            GlobalData g = VELK_GLOBALS;
            VelkSurfaceVertex v0 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.x);
            VelkSurfaceVertex v1 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.y);
            VelkSurfaceVertex v2 = velk_surface_vertex(d, hit.cluster, inst, g, hit.corners.z);
            ctx = velk_surface_context(d, g, v0, v1, v2, vec3(1.0 - hit.weights.x - hit.weights.y, hit.weights), rel);
            // Its corners' normal, on the side `normal` says it is seen from
            // (velk_hit_shading_normal).
            if (dot(ctx.normal, normal) < 0.0) ctx.normal = -ctx.normal;
        }
    }
    ctx.ray_dir = ray_dir;
    ctx.rel_pos = rel;
    ctx.hit_pos = rel + VELK_GLOBALS.cam_pos.xyz;
    return ctx;
}
