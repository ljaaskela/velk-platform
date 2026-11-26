VELK_VARYINGS_IN

VELK_FRAG_OUT(frag_color)
#velk_include VELK_TRACE_FRAGMENT_GLSL

void main()
{
    VelkDrawData d = velk_draw_data.data[pc.draw_base];
    GlobalData globals = velk_globals.data[d.globals_base];
    EvalContext ctx;
    ctx.material_base = d.material_base;
    ctx.texture_id  = d.texture_id;
    ctx.shape_param = v_shape_param;
    ctx.uv          = v_local_uv;
    ctx.uv1         = v_uv1;
    ctx.base        = v_color;
    ctx.ray_dir     = normalize(v_rel_pos);
    ctx.normal      = v_world_normal;
    ctx.hit_pos     = v_rel_pos + globals.cam_pos.xyz;
    ctx.rel_pos     = v_rel_pos;
    ctx.tangent     = v_world_tangent;

    MaterialEval e = <%EVAL_FN%>(ctx);
    if (e.color.a < <%DISCARD%>) discard;

    // An unlit material's alpha is its opacity as it stands, its colour as it is.
    float opacity = e.color.a;
    vec3 color = e.color.rgb;
    if (e.lighting_mode == VELK_LIGHTING_STANDARD) {
        vec3 N = normalize(length(e.normal) > 0.0 ? e.normal : v_world_normal);
        // Seen from both sides: the side it shows is the one toward the camera, of its plane as
        // drawn, as the deferred resolve has it.
        bool two_sided = (d.flags & VELK_DRAW_DOUBLE_SIDED) != 0u;
        if (two_sided) {
            vec3 plane = cross(dFdx(v_rel_pos), dFdy(v_rel_pos));
            if (dot(v_world_normal, plane) * dot(-v_rel_pos, plane) < 0.0) N = -N;
        }
        // Only a thin sheet seen from both sides passes light through.
        float transmission = two_sided ? clamp(e.transmission, 0.0, 1.0) : 0.0;
        vec3 V = normalize(-v_rel_pos);
        float NdotV = max(dot(N, V), 0.0);
        float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
        opacity = e.color.a + fresnel * (1.0 - e.color.a);
        float metallic = clamp(e.metallic, 0.0, 1.0);
        float roughness = clamp(e.roughness, 0.04, 1.0);
        float F90;
        vec3 F0 = velk_specular_f0(e.color.rgb, metallic, e.specular_color_factor, e.specular_factor, F90);

        // The lights its points of light keep and what of each gets through; before they are
        // traced, every light, unshadowed.
        uint point_lights[VELK_LIGHT_SLOTS];
        float point_through[VELK_LIGHT_SLOTS];
        bool points = false;
        uint radiance = VELK_LIGHT_NONE;
        uint radiance_dir = VELK_LIGHT_NONE;
        if (pc.pool_word != VELK_LIGHT_NONE) {
            uvec2 ids = uvec2(pc.draw_base + 1u, (v_instance << d.triangle_bits) | uint(gl_PrimitiveID));
            vec3 b;
            float longest;
            velk_fragment_triangle(d, ids, v_rel_pos, b, longest);
            points = velk_light_direct(pc.pool_word, ids, b, v_rel_pos, longest, pc.light_pixels, point_lights,
                                       point_through, radiance, radiance_dir);
        }
        vec3 direct;
        vec3 highlights;
        vec3 area_irr;
        vec3 area_spec;
        velk_surface_lights(points, point_lights, point_through, v_rel_pos, N, V, F0, F90, roughness, -1.0, transmission, direct,
                            highlights, area_irr, area_spec);

        float env_max_lod = float(textureQueryLevels(velk_textures[nonuniformEXT(pc.env_texture_id)])) - 1.0;
        vec3 env_specular = env_miss_color_lod(reflect(-V, N), roughness * env_max_lod);
        // What its record keeps of the light other surfaces give it, where the pixels that showed
        // it faced about as it does; else the environment around its normal.
        vec3 indirect = env_miss_color_lod(N, env_max_lod);
        if (radiance != VELK_LIGHT_NONE) {
            uint kept = velk_light_cell_words.data[radiance];
            bool facing = radiance_dir == VELK_LIGHT_NONE
                       || dot(N, unpackSnorm4x8(velk_light_cell_words.data[radiance_dir]).xyz) > 0.5;
            if (kept != VELK_LIGHT_NONE && facing) indirect = velk_rgb9e5(kept);
        }
        // A thin sheet passing light through: that share of its diffuse light from its other side.
        if (transmission > 0.0) indirect = mix(indirect, env_miss_color_lod(-N, env_max_lod), transmission);
        // As the deferred lighting's: a normal turned from the camera fades its reflection out.
        vec3 F_env = fresnel_schlick_roughness(NdotV, F0, F90, roughness) * smoothstep(-0.2, 0.0, dot(N, V));
        color = e.color.rgb * (1.0 - metallic) * (direct + indirect + area_irr) + highlights + F_env * env_specular
              + area_spec;
    }
    frag_color = vec4(color + e.emissive, opacity);
    // The culling's trace, at its pixel: drawn over what is there, with this opacity.
    velk_trace_fragment_note(6u, pc.draw_base, 0u, floatBitsToUint(opacity));
}
