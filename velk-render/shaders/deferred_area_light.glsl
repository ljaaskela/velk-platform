// Analytic area-light shading. The clamped-cosine integral over the emitter's
// spherical polygon, evaluated in closed form: no samples, no noise, nothing
// for the denoiser to resolve. Validated against Monte Carlo in
// design-notes/spike_analytic/analytic.cpp before being written here.

// Vertex capacity of every polygon handled here: the light's four corners, which
// clipping at the horizon can grow by one.
const int kOccPolyMax = 8;

// One edge's contribution to the vector form factor.
vec3 velk_ff_edge(vec3 a, vec3 b)
{
    float c = clamp(dot(a, b), -1.0, 1.0);
    vec3  e = cross(a, b);
    float el = length(e);
    return (el > 1e-8) ? e * (acos(c) / el) : vec3(0.0);
}

// Clips a polygon against the half-space dot(nrm, p) >= 0, where the plane
// passes through the shading point. Used for both world-relative points and
// directions: the plane goes through the origin, so clipping the chord and
// clipping the great-circle arc give the same direction once normalised.
int velk_clip_half(inout vec3 p[kOccPolyMax], int n, vec3 nrm)
{
    vec3 o[kOccPolyMax];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        vec3 a = p[i];
        vec3 b = p[i + 1 == n ? 0 : i + 1];
        float da = dot(nrm, a);
        float db = dot(nrm, b);
        if (da >= 0.0 && m < kOccPolyMax) { o[m] = a; ++m; }
        if ((da >= 0.0) != (db >= 0.0) && m < kOccPolyMax) {
            o[m] = mix(a, b, da / (da - db));
            ++m;
        }
    }
    for (int i = 0; i < m; ++i) p[i] = o[i];
    return m;
}

// Vector form factor of a spherical polygon given as directions.
vec3 velk_form_factor(vec3 p[kOccPolyMax], int n)
{
    vec3 f = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        f += velk_ff_edge(normalize(p[i]), normalize(p[i + 1 == n ? 0 : i + 1]));
    }
    return f;
}


// Inverse of the LTC transform for (roughness, N.V), from the fitted table.
// The table stores M as (m00, m02, m11, m20) with m22 == 1, and M has no
// coupling between y and the xz plane, so the inverse is closed-form rather
// than a general 3x3 solve. Returned unnormalised: the polygon integral
// normalises its vertices as directions, so any uniform scale on M cancels.
mat3 velk_ltc_minv(float roughness, float ndotv)
{
    // Table axes match the generator: x = roughness, y = sqrt(1 - N.V), which
    // spends resolution near grazing where the lobe changes fastest.
    vec2 uv = vec2(clamp(roughness, 0.0, 1.0), sqrt(clamp(1.0 - ndotv, 0.0, 1.0)));
    vec4 t = velk_texture(pc.ltc_matrix_id, uv);
    float a = t.x, b = t.y, c = t.z, d = t.w;
    float k = a - b * d;
    if (abs(k) < 1e-6) k = (k < 0.0) ? -1e-6 : 1e-6;
    // Columns, GLSL-style: mat3(col0, col1, col2).
    return mat3(vec3(1.0, 0.0, -d),
                vec3(0.0, k / max(c, 1e-6), 0.0),
                vec3(-b, 0.0, a));
}

// Diffuse irradiance (.x) and the LTC specular integral (.y) from a square area light at
// `world_pos`, with nothing in the way. What blocks it comes from the points of light, as for
// any light.
vec2 velk_area_irradiance(Light light, vec3 world_pos, vec3 N, vec3 V, float roughness)
{
    // Shading frame with N as +Z and the view in the +XZ half-plane. The
    // diffuse integral only reads .z so it does not care about the rotation
    // about N, while LTC requires exactly this frame - so one frame serves
    // both and every polygon is built once.
    vec3 nt = V - N * dot(V, N);
    nt = (dot(nt, nt) > 1e-12) ? normalize(nt)
                              : ((abs(N.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), N))
                                                  : normalize(cross(vec3(1.0, 0.0, 0.0), N)));
    vec3 nb = cross(N, nt);

    vec3 ln = normalize(light.direction.xyz);
    vec3 lt = (abs(ln.y) < 0.9) ? normalize(cross(vec3(0.0, 1.0, 0.0), ln))
                                : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
    vec3 lb = cross(ln, lt);
    float hs = max(light.params.w, 1e-4);

    vec3 ctr = light.position.xyz - world_pos;
    vec3 ea = lt * hs;
    vec3 eb = lb * hs;

    vec3 lw[4];
    lw[0] = ctr - ea - eb;
    lw[1] = ctr + ea - eb;
    lw[2] = ctr + ea + eb;
    lw[3] = ctr - ea + eb;

    vec3 lp[kOccPolyMax];
    for (int i = 0; i < 4; ++i) {
        lp[i] = vec3(dot(lw[i], nt), dot(lw[i], nb), dot(lw[i], N));
    }
    // Unclipped corners are kept: the specular polygon is transformed from
    // these and clipped in ITS own space, since the horizon that matters there
    // is the transformed one.
    vec3 lraw[4];
    for (int i = 0; i < 4; ++i) {
        lraw[i] = vec3(dot(lw[i], nt), dot(lw[i], nb), dot(lw[i], N));
        lp[i] = lraw[i];
    }
    int ln_count = velk_clip_half(lp, 4, vec3(0.0, 0.0, 1.0));
    if (ln_count < 3) return vec2(0.0);

    vec3 f_total = velk_form_factor(lp, ln_count);
    float f_max = abs(f_total.z);

    // Specular: the same emitter through the LTC transform. A zero table id
    // means the fit is unavailable, and the light stays diffuse-only rather
    // than losing its highlight to a black or garbage lookup.
    mat3 minv = mat3(1.0);
    vec3 lps[kOccPolyMax];
    int ls_count = 0;
    float s_max = 0.0;
    if (pc.ltc_matrix_id != 0u) {
        minv = velk_ltc_minv(roughness, clamp(dot(N, V), 0.0, 1.0));
        for (int i = 0; i < 4; ++i) lps[i] = minv * lraw[i];
        ls_count = velk_clip_half(lps, 4, vec3(0.0, 0.0, 1.0));
        if (ls_count >= 3) s_max = abs(velk_form_factor(lps, ls_count).z);
    }

    return vec2(f_max, s_max) / (2.0 * 3.14159265);
}
