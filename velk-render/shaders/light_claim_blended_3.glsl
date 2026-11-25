void main()
{
    // A helper invocation's atomics give nothing back to go by.
    if (gl_HelperInvocation || velk_light_claim_idle()) return;
    VelkDrawData d = velk_draw_data.data[pc.draw_base];
    g_ids = uvec2(pc.draw_base + 1u, (v_instance << d.triangle_bits) | uint(gl_PrimitiveID));
    uint record = velk_ids_record(d, g_ids);
    if (record == VELK_NO_RECORD) return;
    velk_fragment_triangle(d, g_ids, v_rel_pos, g_weights, g_longest);
    velk_light_claim(d, g_ids, record);
}
