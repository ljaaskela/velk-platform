const int kMaxBounces = 4;

vec3 trace_bounce(Ray ray, vec3 throughput)
{
    vec3 acc = vec3(0.0);
    for (int d = 1; d < kMaxBounces; ++d) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            acc += throughput * env_miss_color(ray.dir);
            return acc;
        }
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, velk_hit_shading_normal(hit, ray.dir), material);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        float a = clamp(bs.emission.a, 0.0, 1.0);
        acc += throughput * bs.emission.rgb * a;
        if (bs.terminate) {
            if (a >= 0.999) return acc;
            // Transparent: continue straight through, reduced throughput.
            throughput *= (1.0 - a);
            ray.origin = ctx.rel_pos +ray.dir * 1e-3;
            continue;
        }
        throughput *= bs.throughput;
        ray.origin = ctx.rel_pos +hit.normal * 1e-3;
        ray.dir = bs.next_dir;
    }
    // Bounce budget exhausted. The remaining throughput is illuminated by
    // the environment along whatever direction we were about to trace;
    // without this the mirror-of-mirror chain would blackhole.
    acc += throughput * env_miss_color(ray.dir);
    return acc;
}

// What a primary ray sees: the nearest surface lit, and through a translucent one what is
// behind it, blended by its alpha.
vec3 trace_primary(Ray ray)
{
    const int kMaxLayers = 8;
    vec3 accum = vec3(0.0);
    vec3 throughput = vec3(1.0);
    for (int layer = 0; layer < kMaxLayers; ++layer) {
        RayHit hit;
        if (!trace_closest_hit(ray, hit)) {
            return accum + throughput * env_miss_color(ray.dir);
        }
        uint material;
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, velk_hit_shading_normal(hit, ray.dir), material);
        BrdfSample bs = velk_resolve_fill(material, ctx);

        vec3 rgb = bs.emission.rgb;
        if (!bs.terminate) {
            // Per-hit sample count: material's preferred count (scales with roughness / lobe
            // width), clamped by the tracer's global cap. Mirror surfaces collapse to 1; rough
            // surfaces spend the budget to flatten GGX noise.
            const uint kSppCap = 12u;
            uint spp = clamp(bs.sample_count_hint, 1u, kSppCap);
            vec3 bounce = vec3(0.0);
            Ray refl;
            refl.origin = ctx.rel_pos + hit.normal * 1e-3;
            refl.dir = bs.next_dir;
            bounce += trace_bounce(refl, bs.throughput);
            for (uint sp = 1u; sp < spp; ++sp) {
                BrdfSample bs2 = velk_resolve_fill(material, ctx);
                refl.dir = bs2.next_dir;
                bounce += trace_bounce(refl, bs2.throughput);
            }
            rgb += bounce * (1.0 / float(spp));
        }
        float a = clamp(bs.emission.a, 0.0, 1.0);
        accum += throughput * rgb * a;
        if (a >= 0.999) return accum;
        throughput *= 1.0 - a;
        ray.origin = ctx.rel_pos + ray.dir * 1e-3;
    }
    return accum;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint w = pc.extras.y;
    uint h = pc.extras.z;
    if (coord.x >= int(w) || coord.y >= int(h)) return;

    rng_init(uvec2(coord), VELK_GLOBALS.present_counter);

    // Per-pixel primary sample count. Each sample fires one jittered
    // primary ray; the results are averaged. Reduces edge-aliasing on shape boundaries and damps
    // bounce noise from rough specular hits. Cost scales linearly
    // with kPrimarySamples; 1 = no AA (raw stochastic output),
    // 4 = typical quality / cost balance.
    const uint kPrimarySamples = 4u;

    vec3 final = vec3(0.0);
    for (uint psample = 0u; psample < kPrimarySamples; ++psample) {
        // Sub-pixel jitter. Uniform 0..1 offset into the pixel; with
        // multiple samples the average lands near the pixel center
        // while still anti-aliasing edges. RNG is seeded per coord
        // + frame so jitter patterns are stable within a frame and
        // different between frames.
        vec2 jitter = vec2(rng_next_float(), rng_next_float());
        vec2 ndc = (vec2(coord) + jitter) / vec2(float(w), float(h)) * 2.0 - 1.0;
        vec4 near_h = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 1.0, 1.0);
        vec4 far_h  = VELK_GLOBALS.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 near_w = near_h.xyz / near_h.w;
        vec3 far_w  = far_h.xyz  / far_h.w;

        Ray primary;
        primary.origin = near_w;
        primary.dir = normalize(far_w - near_w);
        final += trace_primary(primary);
    }

    imageStore(gStorageImages[nonuniformEXT(pc.extras.x)], coord,
               vec4(final * (1.0 / float(kPrimarySamples)), 1.0));
}
