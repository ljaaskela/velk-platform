vec3 velk_claim_weights() { return g_weights; }

float velk_claim_tri_size()
{
    return g_longest;
}

float velk_claim_px_per_world(GlobalData g)
{
    mat4 vp = g.view_projection;
    return length(vec3(vp[0][1], vp[1][1], vp[2][1])) * g.viewport.y * 0.5 / max(length(v_rel_pos), 1e-4);
}

float velk_claim_tri_px(GlobalData g)
{
    return velk_claim_tri_size() * velk_claim_px_per_world(g);
}

mat4 velk_claim_world(VelkDrawData d, uvec2 ids)
{
    ElementInstance inst = velk_draw_instance(d, velk_ids_slot(d, ids));
    return velk_sized_world(inst.world_matrix, inst.offset.xyz, inst.size.xyz);
}
