// Half of each side of a square area light facing `facing`, `half_size` across from its
// middle: the same axes velk_area_irradiance uses.
void velk_area_axes(vec3 facing, float half_size, out vec3 a, out vec3 b)
{
    vec3 ln = normalize(facing);
    vec3 lt = abs(ln.y) < 0.9 ? normalize(cross(vec3(0.0, 1.0, 0.0), ln)) : normalize(cross(vec3(1.0, 0.0, 0.0), ln));
    a = lt * half_size;
    b = cross(ln, lt) * half_size;
}

// The diffuse light reaching a surface at `pos` facing `N` from a square area light with
// nothing in the way, as a factor on the light's colour times intensity: on the same scale as
// a point light's N.L, as velk_area_irradiance's diffuse is used.
float velk_area_diffuse(vec3 middle, vec3 facing, float half_size, vec3 pos, vec3 N)
{
    vec3 a;
    vec3 b;
    velk_area_axes(facing, half_size, a, b);
    vec3 c = middle - pos;
    vec3 p[4] = vec3[4](c - a - b, c + a - b, c + a + b, c - a + b);
    // Only what is above the surface's horizon: clipped there, up to five corners.
    vec3 q[5];
    int m = 0;
    for (int i = 0; i < 4; ++i) {
        vec3 u = p[i];
        vec3 v = p[(i + 1) & 3];
        float du = dot(N, u);
        float dv = dot(N, v);
        if (du >= 0.0 && m < 5) q[m++] = u;
        if ((du >= 0.0) != (dv >= 0.0) && m < 5) q[m++] = mix(u, v, du / (du - dv));
    }
    if (m < 3) return 0.0;
    vec3 f = vec3(0.0);
    for (int i = 0; i < m; ++i) {
        vec3 u = normalize(q[i]);
        vec3 v = normalize(q[i + 1 == m ? 0 : i + 1]);
        vec3 e = cross(u, v);
        float el = length(e);
        if (el > 1e-8) f += e * (acos(clamp(dot(u, v), -1.0, 1.0)) / el);
    }
    return abs(dot(f, N)) * 0.5;
}
