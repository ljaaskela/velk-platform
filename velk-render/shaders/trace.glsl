#define VELK_TRACE_NONE 0xFFFFFFFFu
#define VELK_TRACE_CHOOSE 1u
#define VELK_TRACE_PARTITION 2u
#define VELK_TRACE_DRAW 3u

// The ray through the traced pixel, from the camera; false when nothing is traced.
bool velk_trace_ray(GlobalData g, out vec3 origin, out vec3 dir)
{
    origin = vec3(0.0);
    dir = vec3(0.0, 0.0, 1.0);
    uint t = g.trace;
    if (t == VELK_TRACE_NONE) return false;
    vec2 ndc = (vec2(float(cull_words.data[t]), float(cull_words.data[t + 1u])) + 0.5) * g.viewport.zw * 2.0 - 1.0;
    vec4 near_h = g.inverse_view_projection * vec4(ndc, 1.0, 1.0);
    vec4 mid_h = g.inverse_view_projection * vec4(ndc, 0.5, 1.0);
    origin = near_h.xyz / near_h.w;
    dir = normalize(mid_h.xyz / mid_h.w - origin);
    return true;
}

// Whether that ray passes within `radius` of `center`, from the camera.
bool velk_trace_sphere(GlobalData g, vec3 center, float radius)
{
    vec3 o;
    vec3 d;
    if (!velk_trace_ray(g, o, d)) return false;
    vec3 oc = center - o;
    return length(oc - d * max(dot(oc, d), 0.0)) <= radius;
}

// Whether that ray passes the box from `lo` to `hi`, from the camera.
bool velk_trace_box(GlobalData g, vec3 lo, vec3 hi)
{
    vec3 o;
    vec3 d;
    if (!velk_trace_ray(g, o, d)) return false;
    vec3 inv = 1.0 / d;
    vec3 t0 = (lo - o) * inv;
    vec3 t1 = (hi - o) * inv;
    vec3 tn = min(t0, t1);
    vec3 tf = max(t0, t1);
    return min(min(tf.x, tf.y), tf.z) >= max(max(max(tn.x, tn.y), tn.z), 0.0);
}

void velk_trace_note(GlobalData g, uint stage, uint outcome, uint a, uint b, uint c)
{
    uint t = g.trace;
    if (t == VELK_TRACE_NONE) return;
    uint at = atomicAdd(cull_words.data[t + 2u], 1u);
    if (at >= cull_words.data[t + 3u]) return;
    uint w = t + 6u + at * 4u;
    cull_words.data[w] = stage | (outcome << 8u);
    cull_words.data[w + 1u] = a;
    cull_words.data[w + 2u] = b;
    cull_words.data[w + 3u] = c;
}
