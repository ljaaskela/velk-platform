// The brightest the sky may count. Readouts lower it so that a tiny sun cannot dominate them.
float reference_sky_cap = 1e30;

vec3 reference_sky(vec3 rd)
{
    if (pc.env.y == 0u) return vec3(0.0);
    float rotation = uintBitsToFloat(pc.env.w);
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 dir = vec3(c * rd.x + s * rd.z, rd.y, -s * rd.x + c * rd.z);
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.28318531 + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5);
    return min(textureLod(velk_textures[nonuniformEXT(pc.env.y)], uv, 0.0).rgb * uintBitsToFloat(pc.env.z), vec3(reference_sky_cap));
}

vec3 reference_cosine(vec3 n)
{
    vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    float v = rng_next_float();
    float angle = rng_next_float() * 6.28318531;
    float radius = sqrt(v);
    return t * (radius * cos(angle)) + b * (radius * sin(angle)) + n * sqrt(1.0 - v);
}

// One light per surface, in proportion to what it adds there.
void reference_pick_light(vec3 pos, vec3 n)
{
    velk_single_light = -1;
    velk_single_light_weight = 1.0;
    float w_sum = 0.0;
    float w_chosen = 0.0;
    for (uint li = 0u; li < pc.light_count; ++li) {
        Light light = velk_light_at(pc.lights_base + li);
        vec3 L;
        float atten;
        velk_light_sample(light, pos, L, atten);
        vec3 rad = light.color_intensity.rgb * light.color_intensity.a;
        float lw = (rad.r + rad.g + rad.b) * atten * max(dot(n, L), 0.05);
        if (lw <= 0.0) continue;
        w_sum += lw;
        if (rng_next_float() * w_sum <= lw) {
            velk_single_light = int(li);
            w_chosen = lw;
        }
    }
    if (w_chosen > 0.0) velk_single_light_weight = w_sum / w_chosen;
}

float reference_luma(vec3 c)
{
    return c.r + c.g + c.b;
}

// The light a path brings back, and in `first` what its first hit or miss alone brought.
vec3 reference_path(Ray ray, out vec3 first)
{
    const uint kBounces = 12u;
    first = vec3(0.0);
    vec3 total = vec3(0.0);
    vec3 carried = vec3(1.0);
    velk_shade_highlights = true;
    for (uint bounce = 0u; bounce < kBounces; ++bounce) {
        RayHit hit;
        if (!trace_closest_hit(ray, 1e30, hit)) {
            total += carried * reference_sky(ray.dir);
            if (bounce == 0u) first = total;
            break;
        }
        uint material;
        vec3 shading = velk_hit_shading_normal(hit, ray.dir);
        EvalContext ctx = velk_hit_context(hit, ray.dir, ray.origin + hit.t * ray.dir, shading, material);

        reference_pick_light(ctx.rel_pos, shading);
        velk_hit_diffuse = vec3(0.0);
        velk_hit_transmission = 0.0;
        BrdfSample bs = velk_resolve_fill(material, ctx);

        // Partly transparent: either the ray goes straight through, or the surface counts in full.
        if (rng_next_float() >= clamp(bs.emission.a, 0.0, 1.0)) {
            ray.origin = ctx.rel_pos + ray.dir * 0.01;
            continue;
        }
        total += carried * bs.emission.rgb;
        if (bounce == 0u) first = total;
        if (bs.terminate) break;

        // The side of its plane the ray came from, and the normal it was shaded with turned to it.
        vec3 side = dot(hit.plane, ray.dir) > 0.0 ? -hit.plane : hit.plane;
        vec3 n = dot(shading, side) < 0.0 ? -shading : shading;
        float diffuse_part = reference_luma(velk_hit_diffuse);
        float specular_part = reference_luma(bs.throughput);
        if (diffuse_part + specular_part < 1e-4) break;
        float specular_share = specular_part / (diffuse_part + specular_part);
        if (rng_next_float() < specular_share) {
            carried *= bs.throughput / specular_share;
            ray.dir = bs.next_dir;
            if (dot(ray.dir, side) <= 0.0) break;
        } else {
            carried *= velk_hit_diffuse / (1.0 - specular_share);
            velk_shade_highlights = false;
            // Through a thin sheet as often as it passes light through: to its other side.
            if (rng_next_float() < velk_hit_transmission) {
                n = -n;
                side = -side;
            }
            ray.dir = reference_cosine(n);
            if (dot(ray.dir, side) <= 0.0) break;
        }
        ray.origin = ctx.rel_pos + side * 0.01;
        if (max(carried.r, max(carried.g, carried.b)) < 1e-3) break;
    }
    return total;
}
