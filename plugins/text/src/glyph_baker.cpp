#include "glyph_baker.h"

#include <algorithm>
#include <cstring>

namespace velk::ui {

namespace {

// State threaded through FreeType outline-decompose callbacks.
struct DecomposeState
{
    vector<QuadCurve>* curves;
    vec2 current; // pen position in font units
    bool cubic_seen;
};

// With FT_LOAD_NO_SCALE the outline is in raw font units stored as FT_Pos
// (long), not 26.6 fixed point - just cast.
vec2 from_ft(const FT_Vector* v)
{
    return {static_cast<float>(v->x), static_cast<float>(v->y)};
}

int cb_move_to(const FT_Vector* to, void* user)
{
    auto* s = static_cast<DecomposeState*>(user);
    s->current = from_ft(to);
    return 0;
}

int cb_line_to(const FT_Vector* to, void* user)
{
    auto* s = static_cast<DecomposeState*>(user);
    vec2 p2 = from_ft(to);
    // Degenerate quadratic: control point at the midpoint of p0 and p2.
    QuadCurve c{};
    c.p0 = s->current;
    c.p1 = {(s->current.x + p2.x) * 0.5f, (s->current.y + p2.y) * 0.5f};
    c.p2 = p2;
    s->curves->push_back(c);
    s->current = p2;
    return 0;
}

int cb_conic_to(const FT_Vector* control, const FT_Vector* to, void* user)
{
    auto* s = static_cast<DecomposeState*>(user);
    QuadCurve c{};
    c.p0 = s->current;
    c.p1 = from_ft(control);
    c.p2 = from_ft(to);
    s->curves->push_back(c);
    s->current = c.p2;
    return 0;
}

int cb_cubic_to(const FT_Vector* /*c1*/, const FT_Vector* /*c2*/, const FT_Vector* to, void* user)
{
    auto* s = static_cast<DecomposeState*>(user);
    s->cubic_seen = true;
    // Keep the pen consistent so any subsequent decompose steps don't reference stale state.
    s->current = from_ft(to);
    return 0;
}

float min3(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

float max3(float a, float b, float c)
{
    float m = a > b ? a : b;
    return m > c ? m : c;
}

// Sort curves into bands along one axis.
//
// axis = 0: horizontal bands, split by y. Curves are bucketed by the y range
//           they cover, sorted within each band by max x (the "other" axis).
// axis = 1: vertical bands, split by x. Bucketed by x range, sorted by max y.
void assign_bands(
    const vector<QuadCurve>& curves,
    int axis,
    vector<uint16_t>& flat_out,
    uint16_t (&offsets_out)[BakedGlyph::BAND_COUNT + 1])
{
    constexpr uint32_t N = BakedGlyph::BAND_COUNT;

    // Per-band scratch: curve indices and their sort keys.
    // Bands are typically tiny (<16 entries) so a small flat vector each is fine.
    vector<uint16_t> per_band_idx[N];
    vector<float> per_band_key[N];

    const uint16_t curve_count = static_cast<uint16_t>(curves.size());
    for (uint16_t i = 0; i < curve_count; ++i) {
        const QuadCurve& c = curves[i];

        float a0 = (axis == 0) ? c.p0.y : c.p0.x;
        float a1 = (axis == 0) ? c.p1.y : c.p1.x;
        float a2 = (axis == 0) ? c.p2.y : c.p2.x;
        float lo = min3(a0, a1, a2);
        float hi = max3(a0, a1, a2);

        // Map [0, 1] to band indices [0, N), clamping for numerical fuzz.
        int band_lo = static_cast<int>(lo * static_cast<float>(N));
        int band_hi = static_cast<int>(hi * static_cast<float>(N));
        if (band_lo < 0) band_lo = 0;
        if (band_hi < 0) band_hi = 0;
        if (band_lo >= static_cast<int>(N)) band_lo = static_cast<int>(N) - 1;
        if (band_hi >= static_cast<int>(N)) band_hi = static_cast<int>(N) - 1;

        // Sort key: max coordinate on the orthogonal axis. Used by the shader
        // to early-exit when walking the band's curves.
        float b0 = (axis == 0) ? c.p0.x : c.p0.y;
        float b1 = (axis == 0) ? c.p1.x : c.p1.y;
        float b2 = (axis == 0) ? c.p2.x : c.p2.y;
        float key = max3(b0, b1, b2);

        for (int b = band_lo; b <= band_hi; ++b) {
            per_band_idx[b].push_back(i);
            per_band_key[b].push_back(key);
        }
    }

    flat_out.clear();
    offsets_out[0] = 0;
    for (uint32_t b = 0; b < N; ++b) {
        // Insertion sort, descending by key. Bands are tiny so O(n^2) is fine.
        const size_t n = per_band_idx[b].size();
        for (size_t i = 1; i < n; ++i) {
            uint16_t idx = per_band_idx[b][i];
            float key = per_band_key[b][i];
            size_t j = i;
            while (j > 0 && per_band_key[b][j - 1] < key) {
                per_band_idx[b][j] = per_band_idx[b][j - 1];
                per_band_key[b][j] = per_band_key[b][j - 1];
                --j;
            }
            per_band_idx[b][j] = idx;
            per_band_key[b][j] = key;
        }
        for (size_t i = 0; i < n; ++i) {
            flat_out.push_back(per_band_idx[b][i]);
        }
        offsets_out[b + 1] = static_cast<uint16_t>(flat_out.size());
    }
}

} // namespace

GlyphBaker::Result GlyphBaker::bake(FT_Face face, uint32_t glyph_id, BakedGlyph& out)
{
    out.curves.clear();
    out.h_band_curves.clear();
    out.v_band_curves.clear();
    std::memset(out.h_band_offsets, 0, sizeof(out.h_band_offsets));
    std::memset(out.v_band_offsets, 0, sizeof(out.v_band_offsets));
    out.bbox_min = {0.f, 0.f};
    out.bbox_max = {0.f, 0.f};

    // FT_LOAD_NO_SCALE: keep coordinates in font units, no hinting, no rasterization.
    if (FT_Load_Glyph(face, glyph_id, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0) {
        return Result::FreeTypeError;
    }

    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        return Result::FreeTypeError;
    }

    FT_Outline& outline = face->glyph->outline;
    if (outline.n_contours == 0 || outline.n_points == 0) {
        return Result::Empty;
    }

    DecomposeState state{};
    state.curves = &out.curves;

    FT_Outline_Funcs funcs{};
    funcs.move_to = &cb_move_to;
    funcs.line_to = &cb_line_to;
    funcs.conic_to = &cb_conic_to;
    funcs.cubic_to = &cb_cubic_to;
    funcs.shift = 0;
    funcs.delta = 0;

    if (FT_Outline_Decompose(&outline, &funcs, &state) != 0) {
        return Result::FreeTypeError;
    }

    if (state.cubic_seen) {
        out.curves.clear();
        return Result::CubicNotSupported;
    }

    if (out.curves.empty()) {
        return Result::Empty;
    }

    // Compute bbox from collected curves (in font units).
    vec2 mn = out.curves[0].p0;
    vec2 mx = out.curves[0].p0;
    auto extend = [&](const vec2& p) {
        if (p.x < mn.x) mn.x = p.x;
        if (p.y < mn.y) mn.y = p.y;
        if (p.x > mx.x) mx.x = p.x;
        if (p.y > mx.y) mx.y = p.y;
    };
    for (const auto& c : out.curves) {
        extend(c.p0);
        extend(c.p1);
        extend(c.p2);
    }

    const float w = mx.x - mn.x;
    const float h = mx.y - mn.y;
    if (w <= 0.f || h <= 0.f) {
        out.curves.clear();
        return Result::Empty;
    }

    out.bbox_min = mn;
    out.bbox_max = mx;

    // Normalize curves to [0, 1] x [0, 1] over the glyph bbox.
    const float inv_w = 1.f / w;
    const float inv_h = 1.f / h;
    for (auto& c : out.curves) {
        c.p0.x = (c.p0.x - mn.x) * inv_w;
        c.p0.y = (c.p0.y - mn.y) * inv_h;
        c.p1.x = (c.p1.x - mn.x) * inv_w;
        c.p1.y = (c.p1.y - mn.y) * inv_h;
        c.p2.x = (c.p2.x - mn.x) * inv_w;
        c.p2.y = (c.p2.y - mn.y) * inv_h;
    }

    assign_bands(out.curves, 0, out.h_band_curves, out.h_band_offsets);
    assign_bands(out.curves, 1, out.v_band_curves, out.v_band_offsets);

    return Result::Ok;
}

} // namespace velk::ui
