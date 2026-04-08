// Standalone tool that bakes the printable ASCII range from the embedded
// Inter Regular font and prints per-glyph and per-band statistics.
//
// Useful for:
//  - Validating that GlyphBaker produces sensible output for a real font
//  - Empirically tuning BakedGlyph::BAND_COUNT (see "max curves per band"
//    in the summary)
//  - Catching regressions if the baker is later refactored
//
// Run from the build directory after building the glyph_dump target:
//   ./bin/Release/glyph_dump

#include "../src/embedded/inter_regular.h"
#include "../src/font_buffers.h"
#include "../src/glyph_baker.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <cstdint>

using namespace velk::ui;

namespace {

struct Stats
{
    uint32_t glyphs_baked = 0;
    uint32_t glyphs_empty = 0;
    uint32_t glyphs_failed = 0;
    uint32_t total_curves = 0;
    uint32_t max_curves_in_glyph = 0;
    uint32_t max_curves_in_h_band = 0;
    uint32_t max_curves_in_v_band = 0;
    uint32_t total_h_band_entries = 0;
    uint32_t total_v_band_entries = 0;
};

uint32_t max_band_load(
    const uint16_t (&offsets)[BakedGlyph::BAND_COUNT + 1])
{
    uint32_t mx = 0;
    for (uint32_t b = 0; b < BakedGlyph::BAND_COUNT; ++b) {
        uint32_t load = offsets[b + 1] - offsets[b];
        if (load > mx) mx = load;
    }
    return mx;
}

void print_glyph_row(char ch, uint32_t glyph_id, const BakedGlyph& g)
{
    char display = (ch >= 0x20 && ch < 0x7f) ? ch : '?';
    std::printf(
        "  '%c' gid=%-4u curves=%-4zu  h-band-max=%-3u v-band-max=%-3u  bbox=[%.0f,%.0f .. %.0f,%.0f]\n",
        display,
        glyph_id,
        g.curves.size(),
        max_band_load(g.h_band_offsets),
        max_band_load(g.v_band_offsets),
        g.bbox_min.x,
        g.bbox_min.y,
        g.bbox_max.x,
        g.bbox_max.y);
}

} // namespace

int main()
{
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
        std::fprintf(stderr, "FT_Init_FreeType failed\n");
        return 1;
    }

    FT_Face face = nullptr;
    if (FT_New_Memory_Face(
            lib,
            embedded::inter_regular_ttf,
            static_cast<FT_Long>(embedded::inter_regular_ttf_size),
            0,
            &face) != 0)
    {
        std::fprintf(stderr, "FT_New_Memory_Face failed\n");
        FT_Done_FreeType(lib);
        return 1;
    }

    std::printf("Font: %s %s, units_per_EM=%d, num_glyphs=%ld\n",
                face->family_name ? face->family_name : "(unknown)",
                face->style_name ? face->style_name : "",
                face->units_per_EM,
                face->num_glyphs);
    std::printf("Band count per axis: %u\n\n", BakedGlyph::BAND_COUNT);

    GlyphBaker baker;
    BakedGlyph g;
    Stats stats;

    std::printf("Per-glyph (printable ASCII):\n");

    for (char ch = 0x20; ch < 0x7f; ++ch) {
        uint32_t glyph_id = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        if (glyph_id == 0) {
            std::printf("  '%c' (no glyph)\n", ch);
            continue;
        }

        auto result = baker.bake(face, glyph_id, g);
        switch (result) {
        case GlyphBaker::Result::Ok:
            stats.glyphs_baked++;
            stats.total_curves += static_cast<uint32_t>(g.curves.size());
            stats.max_curves_in_glyph =
                std::max(stats.max_curves_in_glyph, static_cast<uint32_t>(g.curves.size()));
            stats.max_curves_in_h_band =
                std::max(stats.max_curves_in_h_band, max_band_load(g.h_band_offsets));
            stats.max_curves_in_v_band =
                std::max(stats.max_curves_in_v_band, max_band_load(g.v_band_offsets));
            stats.total_h_band_entries += static_cast<uint32_t>(g.h_band_curves.size());
            stats.total_v_band_entries += static_cast<uint32_t>(g.v_band_curves.size());
            print_glyph_row(ch, glyph_id, g);
            break;

        case GlyphBaker::Result::Empty:
            stats.glyphs_empty++;
            std::printf("  '%c' gid=%-4u (empty)\n", ch, glyph_id);
            break;

        case GlyphBaker::Result::CubicNotSupported:
            stats.glyphs_failed++;
            std::printf("  '%c' gid=%-4u CUBIC NOT SUPPORTED\n", ch, glyph_id);
            break;

        case GlyphBaker::Result::FreeTypeError:
            stats.glyphs_failed++;
            std::printf("  '%c' gid=%-4u FREETYPE ERROR\n", ch, glyph_id);
            break;
        }
    }

    std::printf("\nSummary:\n");
    std::printf("  baked        : %u\n", stats.glyphs_baked);
    std::printf("  empty        : %u\n", stats.glyphs_empty);
    std::printf("  failed       : %u\n", stats.glyphs_failed);
    std::printf("  total curves : %u\n", stats.total_curves);
    if (stats.glyphs_baked > 0) {
        std::printf("  avg curves/glyph : %.1f\n",
                    static_cast<float>(stats.total_curves) / static_cast<float>(stats.glyphs_baked));
    }
    std::printf("  max curves/glyph : %u\n", stats.max_curves_in_glyph);
    std::printf("  max curves in any horizontal band : %u (of %u bands)\n",
                stats.max_curves_in_h_band, BakedGlyph::BAND_COUNT);
    std::printf("  max curves in any vertical   band : %u (of %u bands)\n",
                stats.max_curves_in_v_band, BakedGlyph::BAND_COUNT);
    std::printf("  total h-band entries (sum over glyphs) : %u\n", stats.total_h_band_entries);
    std::printf("  total v-band entries (sum over glyphs) : %u\n", stats.total_v_band_entries);

    // Now exercise FontBuffers: bake the same range through it and report
    // the resulting buffer sizes (this is what would actually be uploaded
    // to the GPU per font).
    {
        FontBuffers fb;
        uint32_t added = 0;
        uint32_t failed = 0;
        for (char ch = 0x20; ch < 0x7f; ++ch) {
            uint32_t gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
            if (gid == 0) continue;
            uint32_t idx = fb.ensure_glyph(face, gid);
            if (idx == FontBuffers::INVALID_INDEX) {
                failed++;
            } else {
                added++;
            }
        }
        std::printf("\nFontBuffers (printable ASCII):\n");
        std::printf("  glyphs added : %u (failed: %u)\n", added, failed);
        std::printf("  curve buffer : %zu curves, %zu bytes\n",
                    fb.curves_count(), fb.curves_bytes());
        std::printf("  band  buffer : %zu uint32, %zu bytes\n",
                    fb.bands_count(), fb.bands_bytes());
        std::printf("  glyph table  : %zu records, %zu bytes\n",
                    fb.glyphs_count(), fb.glyphs_bytes());
        std::printf("  total upload : %zu bytes\n",
                    fb.curves_bytes() + fb.bands_bytes() + fb.glyphs_bytes());
        std::printf("  dirty flags  : curves=%s bands=%s glyphs=%s\n",
                    fb.curves_dirty() ? "set" : "clear",
                    fb.bands_dirty()  ? "set" : "clear",
                    fb.glyphs_dirty() ? "set" : "clear");

        // Idempotence check: re-baking the same glyphs must not grow the buffers.
        size_t before = fb.curves_count() + fb.bands_count() + fb.glyphs_count();
        for (char ch = 0x20; ch < 0x7f; ++ch) {
            uint32_t gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
            if (gid == 0) continue;
            fb.ensure_glyph(face, gid);
        }
        size_t after = fb.curves_count() + fb.bands_count() + fb.glyphs_count();
        std::printf("  idempotent re-bake : %s (before=%zu, after=%zu)\n",
                    (before == after) ? "ok" : "FAIL", before, after);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return 0;
}
