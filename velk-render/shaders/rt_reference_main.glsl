void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.z;
    uint h = pc.extras.w;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    uint frame = VELK_GLOBALS.present_counter;
    vec4 held = pc.shapes_base != 0u ? vec4(0.0) : texelFetch(velk_textures[nonuniformEXT(pc.extras.x)], coord, 0);

    const uint kShare = 8u;
    if ((uint(coord.x) + uint(coord.y) * 3u + frame) % kShare == 0u) {
        rng_init(uvec2(coord), frame);
        vec2 jitter = vec2(rng_next_float(), rng_next_float());
        vec2 ndc = (vec2(coord) + jitter) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w = far_h.xyz / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);
        vec3 first_hit;
        vec3 brought = reference_path(primary, first_hit);
        if (!any(isnan(brought)) && !any(isinf(brought))) {
            held += vec4(brought, 1.0);
        }
    }

    imageStore(gStorageImagesF32[nonuniformEXT(pc.extras.x)], coord, held);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.extras.y)], coord, vec4(held.rgb / max(held.a, 1.0), 1.0));
}
