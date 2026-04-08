#ifndef VELK_UI_TEXT_VELK_TEXT_GLSL_H
#define VELK_UI_TEXT_VELK_TEXT_GLSL_H

namespace velk::ui::embedded {

// =====================================================================
// velk_text.glsl
//
// Slug-style analytic Bezier glyph coverage. Translated from Eric Lengyel's
// public-domain reference HLSL (https://github.com/EricLengyel/Slug),
// adapted for std430 buffer-reference storage instead of curve/band textures.
//
// Coordinate system convention:
//
//   The function operates in "glyph normalized space" where the curves and
//   the sample uv are both in [0, 1] x [0, 1] over the glyph bbox, with the
//   y axis matching the curve coordinate system. GlyphBaker normalizes
//   curves with FreeType's Y-up convention, so the caller must pass uv with
//   y also pointing up (i.e. uv.y = 0 at the glyph descender, uv.y = 1 at
//   the ascender). If the caller has Y-down quad uv, it must flip y before
//   calling velk_text_coverage.
//
// Buffer layout (matches font_buffers.h):
//
//   - GlyphRecord (32 bytes): bbox_min, bbox_max, curve_offset, curve_count,
//     band_data_offset, _pad
//   - QuadCurve (24 bytes): vec2 p0, p1, p2 (no shared endpoints)
//   - Band buffer is a flat uint array. Per glyph at band_data_offset:
//       [0  .. N]                    h_offsets[N+1] (prefix sums, [0]=0)
//       [N+1 .. N+1+h_total)         h curve indices (relative to curve_offset)
//       [N+1+h_total .. 2N+2+h_total]  v_offsets[N+1]
//       [2N+2+h_total .. ...)        v curve indices (relative to curve_offset)
//     where N = VELK_TEXT_BAND_COUNT and h_total = h_offsets[N].
//
// Curve indices stored in the band buffer are RELATIVE to the glyph (add
// glyph.curve_offset to get an index into the curves[] buffer).
//
// Band sort convention (matches GlyphBaker::assign_bands):
//   - Horizontal band i covers y in [i/N, (i+1)/N]. Curves within the band
//     are sorted descending by max x. The shader casts a horizontal ray
//     toward +x from the sample and counts curve crossings of y = sample.y.
//   - Vertical band i covers x in [i/N, (i+1)/N]. Curves sorted descending
//     by max y. Vertical ray cast toward +y, count crossings of x = sample.x.
// =====================================================================

inline constexpr char velk_text_glsl[] = R"(
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

const uint VELK_TEXT_BAND_COUNT = 8u;

struct VelkTextQuadCurve {
    vec2 p0;
    vec2 p1;
    vec2 p2;
};

struct VelkTextGlyphRecord {
    vec2 bbox_min;
    vec2 bbox_max;
    uint curve_offset;
    uint curve_count;
    uint band_data_offset;
    uint _pad;
};

layout(buffer_reference, std430) buffer VelkTextCurveBuffer {
    VelkTextQuadCurve data[];
};

layout(buffer_reference, std430) buffer VelkTextBandBuffer {
    uint data[];
};

layout(buffer_reference, std430) buffer VelkTextGlyphBuffer {
    VelkTextGlyphRecord data[];
};

// Bit-hack root eligibility table from Lengyel's reference. Returns a 2-bit
// code indicating which roots of the sample-relative quadratic in y can
// produce a real intersection of y = 0 inside the curve.
uint velk_text_calc_root_code(float y0, float y1, float y2)
{
    uint i0 = floatBitsToUint(y0) >> 31u;
    uint i1 = floatBitsToUint(y1) >> 30u;
    uint i2 = floatBitsToUint(y2) >> 29u;
    uint shift = (i1 & 2u) | (i0 & ~2u);
    shift = (i2 & 4u) | (shift & ~4u);
    return ((0x2E74u >> shift) & 0x0101u);
}

// Solve for the x values where the sample-relative quadratic crosses y = 0.
// Curve points must already be sample-relative (sample uv subtracted).
vec2 velk_text_solve_horiz(vec2 p0, vec2 p1, vec2 p2)
{
    vec2 a = p0 - 2.0 * p1 + p2;
    vec2 b = p0 - p1;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;

    float d = sqrt(max(b.y * b.y - a.y * p0.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    // Near-linear case: solve -2 b t + c = 0 instead.
    if (abs(a.y) < 1.0 / 65536.0) {
        t1 = p0.y * rb;
        t2 = t1;
    }

    return vec2(
        (a.x * t1 - 2.0 * b.x) * t1 + p0.x,
        (a.x * t2 - 2.0 * b.x) * t2 + p0.x);
}

// Solve for the y values where the sample-relative quadratic crosses x = 0.
vec2 velk_text_solve_vert(vec2 p0, vec2 p1, vec2 p2)
{
    vec2 a = p0 - 2.0 * p1 + p2;
    vec2 b = p0 - p1;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;

    float d = sqrt(max(b.x * b.x - a.x * p0.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    if (abs(a.x) < 1.0 / 65536.0) {
        t1 = p0.x * rb;
        t2 = t1;
    }

    return vec2(
        (a.y * t1 - 2.0 * b.y) * t1 + p0.y,
        (a.y * t2 - 2.0 * b.y) * t2 + p0.y);
}

// Combine horizontal and vertical ray coverages with their fade weights.
// Absolute values let either winding direction work.
float velk_text_combine_coverage(float xcov, float ycov, float xwgt, float ywgt)
{
    float weight_sum = max(xwgt + ywgt, 1.0 / 65536.0);
    float coverage = max(
        abs(xcov * xwgt + ycov * ywgt) / weight_sum,
        min(abs(xcov), abs(ycov)));
    return clamp(coverage, 0.0, 1.0);
}

// Main entry point. Returns coverage in [0, 1] for the sample uv inside the
// glyph identified by glyph_index in the glyphs buffer.
//
// The buffer_reference declarations are intentionally not `readonly`:
// SPIR-V's NonWritable decoration is not legal on a function parameter
// that is a pointer to a PhysicalStorageBuffer, so the parameter qualifier
// would have to match by also being `readonly`, which the validator
// rejects. Dropping the qualifier on both sides keeps the function
// portable; the buffers are still effectively read-only because nothing
// in this shader writes to them.
float velk_text_coverage(
    vec2 uv,
    uint glyph_index,
    VelkTextCurveBuffer curves,
    VelkTextBandBuffer bands,
    VelkTextGlyphBuffer glyphs)
{
    VelkTextGlyphRecord g = glyphs.data[glyph_index];

    if (g.curve_count == 0u) {
        return 0.0;
    }

    // fwidth(uv) gives bbox-normalized units per pixel; the reciprocal gives
    // pixels per bbox unit, used as the AA scale and the early-exit threshold.
    vec2 ems_per_pixel = max(fwidth(uv), vec2(1e-20));
    vec2 pixels_per_em = 1.0 / ems_per_pixel;

    // Pick which horizontal and vertical bands this sample falls into.
    int hband = clamp(int(uv.y * float(VELK_TEXT_BAND_COUNT)),
                      0, int(VELK_TEXT_BAND_COUNT) - 1);
    int vband = clamp(int(uv.x * float(VELK_TEXT_BAND_COUNT)),
                      0, int(VELK_TEXT_BAND_COUNT) - 1);

    uint base = g.band_data_offset;
    uint h_offset_lo = bands.data[base + uint(hband)];
    uint h_offset_hi = bands.data[base + uint(hband) + 1u];
    uint h_total     = bands.data[base + VELK_TEXT_BAND_COUNT];
    uint h_curves_base = base + VELK_TEXT_BAND_COUNT + 1u;

    uint v_offsets_base = h_curves_base + h_total;
    uint v_offset_lo = bands.data[v_offsets_base + uint(vband)];
    uint v_offset_hi = bands.data[v_offsets_base + uint(vband) + 1u];
    uint v_curves_base = v_offsets_base + VELK_TEXT_BAND_COUNT + 1u;

    float xcov = 0.0;
    float xwgt = 0.0;

    // Horizontal band: ray toward +x. Curves sorted descending by max x;
    // bail when the largest x in the curve falls left of the sample by more
    // than half a pixel.
    for (uint i = h_offset_lo; i < h_offset_hi; ++i) {
        uint local_idx = bands.data[h_curves_base + i];
        VelkTextQuadCurve c = curves.data[g.curve_offset + local_idx];
        vec2 p0 = c.p0 - uv;
        vec2 p1 = c.p1 - uv;
        vec2 p2 = c.p2 - uv;

        if (max(max(p0.x, p1.x), p2.x) * pixels_per_em.x < -0.5) break;

        uint code = velk_text_calc_root_code(p0.y, p1.y, p2.y);
        if (code != 0u) {
            vec2 r = velk_text_solve_horiz(p0, p1, p2) * pixels_per_em.x;
            if ((code & 1u) != 0u) {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u) {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float ycov = 0.0;
    float ywgt = 0.0;

    // Vertical band: ray toward +y. Curves sorted descending by max y.
    for (uint i = v_offset_lo; i < v_offset_hi; ++i) {
        uint local_idx = bands.data[v_curves_base + i];
        VelkTextQuadCurve c = curves.data[g.curve_offset + local_idx];
        vec2 p0 = c.p0 - uv;
        vec2 p1 = c.p1 - uv;
        vec2 p2 = c.p2 - uv;

        if (max(max(p0.y, p1.y), p2.y) * pixels_per_em.y < -0.5) break;

        uint code = velk_text_calc_root_code(p0.x, p1.x, p2.x);
        if (code != 0u) {
            vec2 r = velk_text_solve_vert(p0, p1, p2) * pixels_per_em.y;
            if ((code & 1u) != 0u) {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u) {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    return velk_text_combine_coverage(xcov, ycov, xwgt, ywgt);
}
)";

} // namespace velk::ui::embedded

#endif // VELK_UI_TEXT_VELK_TEXT_GLSL_H
