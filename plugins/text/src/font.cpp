#include "font.h"

#include "embedded/inter_regular.h"
#include "font_gpu_buffer.h"
#include "visual/text_material.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <cstring>

namespace velk::ui::impl {

Font::Font() = default;

Font::~Font()
{
    if (hb_buffer_) {
        hb_buffer_destroy(hb_buffer_);
        hb_buffer_ = nullptr;
    }
    if (hb_font_) {
        hb_font_destroy(hb_font_);
        hb_font_ = nullptr;
    }
    if (ft_face_) {
        FT_Done_Face(ft_face_);
        ft_face_ = nullptr;
    }
    if (ft_library_) {
        FT_Done_FreeType(ft_library_);
        ft_library_ = nullptr;
    }
}

void Font::init_buffers()
{
    auto& instance = ::velk::instance();
    curve_buffer_ = instance.create<IBuffer>(ClassId::FontGpuBuffer);
    band_buffer_  = instance.create<IBuffer>(ClassId::FontGpuBuffer);
    glyph_buffer_ = instance.create<IBuffer>(ClassId::FontGpuBuffer);
    if (auto i = interface_cast<IFontGpuBufferInternal>(curve_buffer_)) {
        i->init(&font_buffers_, FontGpuBufferRole::Curves);
    }
    if (auto i = interface_cast<IFontGpuBufferInternal>(band_buffer_)) {
        i->init(&font_buffers_, FontGpuBufferRole::Bands);
    }
    if (auto i = interface_cast<IFontGpuBufferInternal>(glyph_buffer_)) {
        i->init(&font_buffers_, FontGpuBufferRole::Glyphs);
    }

    // The font owns one TextMaterial bound to its three GPU buffers. Every
    // text visual using this font shares this material instance, which is
    // what lets the renderer batch them into a single draw call.
    text_material_ = instance.create<IMaterial>(ClassId::TextMaterial);
    if (auto m = interface_cast<ITextMaterialInternal>(text_material_)) {
        m->set_font_buffers(curve_buffer_, band_buffer_, glyph_buffer_);
    }
}

bool Font::init_from_memory(const uint8_t* data, uint32_t size)
{
    // Keep a copy: FreeType requires font data to stay alive
    font_data_.resize(size);
    std::memcpy(font_data_.data(), data, size);

    if (FT_Init_FreeType(&ft_library_) != 0) {
        return false;
    }

    if (FT_New_Memory_Face(
            ft_library_, font_data_.data(), static_cast<FT_Long>(font_data_.size()), 0, &ft_face_) != 0) {
        return false;
    }

    // Configure FreeType so that ppem == units_per_em. With dpi = 72,
    // ppem = char_size_pts, so char_size_pts = units_per_em. The 26.6 fixed
    // value is units_per_em * 64. This makes hb_ft_font_create set up the
    // HarfBuzz font with a scale that returns shaping advances in font
    // units (specifically in 1/64 of font units, which we then divide by
    // 64 in shape_text). Glyph baking still uses FT_LOAD_NO_SCALE so it
    // ignores this setting and reads raw outline coordinates.
    const FT_Long upem = ft_face_->units_per_EM;
    const FT_F26Dot6 reference_char_size = static_cast<FT_F26Dot6>(upem * 64);
    if (FT_Set_Char_Size(ft_face_, 0, reference_char_size, 72, 72) != 0) {
        return false;
    }

    hb_font_ = hb_ft_font_create(ft_face_, nullptr);
    if (!hb_font_) {
        return false;
    }

    hb_buffer_ = hb_buffer_create();
    if (!hb_buffer_) {
        return false;
    }

    init_buffers();

    // Read design-unit metrics directly from the face. These are constants
    // for the lifetime of the font; the visual scales them per-call.
    if (auto writer = write_state<IFont>(this)) {
        writer->units_per_em = static_cast<float>(upem);
        writer->ascender = static_cast<float>(ft_face_->ascender);
        writer->descender = static_cast<float>(ft_face_->descender);
        writer->line_height = static_cast<float>(ft_face_->height);
    }

    return true;
}

bool Font::init_default()
{
    return init_from_memory(embedded::inter_regular_ttf, embedded::inter_regular_ttf_size);
}

float Font::shape_text(string_view text, vector<IFont::GlyphPosition>& out)
{
    out.clear();

    if (!hb_font_ || !hb_buffer_ || text.empty()) {
        return 0.f;
    }

    hb_buffer_reset(hb_buffer_);
    hb_buffer_add_utf8(hb_buffer_, text.data(), static_cast<int>(text.size()), 0, -1);
    hb_buffer_set_direction(hb_buffer_, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buffer_, HB_SCRIPT_LATIN);
    hb_buffer_set_language(hb_buffer_, hb_language_from_string("en", -1));

    hb_shape(hb_font_, hb_buffer_, nullptr, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t* glyph_info = hb_buffer_get_glyph_infos(hb_buffer_, &glyph_count);
    hb_glyph_position_t* glyph_pos = hb_buffer_get_glyph_positions(hb_buffer_, &glyph_count);

    float total_advance = 0.f;
    float scale = 1.f / 64.f;

    for (unsigned int i = 0; i < glyph_count; ++i) {
        GlyphPosition gp;
        gp.glyph_id = glyph_info[i].codepoint;
        gp.offset.x = static_cast<float>(glyph_pos[i].x_offset) * scale;
        gp.offset.y = static_cast<float>(glyph_pos[i].y_offset) * scale;
        gp.advance.x = static_cast<float>(glyph_pos[i].x_advance) * scale;
        gp.advance.y = static_cast<float>(glyph_pos[i].y_advance) * scale;
        out.push_back(gp);
        total_advance += gp.advance.x;
    }

    return total_advance;
}

IFont::GlyphInfo Font::ensure_glyph(uint32_t glyph_id)
{
    GlyphInfo info{};
    info.internal_index = FontBuffers::INVALID_INDEX;

    if (!ft_face_) {
        return info;
    }

    uint32_t idx = font_buffers_.ensure_glyph(ft_face_, glyph_id);
    if (idx == FontBuffers::INVALID_INDEX) {
        return info;
    }

    const GlyphRecord* rec = font_buffers_.glyph_record(idx);
    if (!rec) {
        return info;
    }

    info.internal_index = idx;
    info.bbox_min = rec->bbox_min;
    info.bbox_max = rec->bbox_max;
    info.empty = (rec->curve_count == 0);
    return info;
}

void Font::layout_line_glyphs(string_view text, float scale, float ascender_px,
                              float baseline_y, TextLayoutResult& out)
{
    if (text.empty()) {
        return;
    }

    vector<GlyphPosition> positions;
    shape_text(text, positions);

    float cursor_x = 0.f;
    uint32_t first = static_cast<uint32_t>(out.glyphs.size());

    for (auto& gp : positions) {
        GlyphInfo info = ensure_glyph(gp.glyph_id);
        if (info.empty) {
            cursor_x += gp.advance.x * scale;
            continue;
        }

        const float bearing_x_px = info.bbox_min.x * scale;
        const float bearing_y_px = info.bbox_max.y * scale;
        const float glyph_w_px = (info.bbox_max.x - info.bbox_min.x) * scale;
        const float glyph_h_px = (info.bbox_max.y - info.bbox_min.y) * scale;

        PositionedGlyph pg{};
        pg.pos.x = cursor_x + gp.offset.x * scale + bearing_x_px;
        pg.pos.y = baseline_y + ascender_px - bearing_y_px + gp.offset.y * scale;
        pg.size = {glyph_w_px, glyph_h_px};
        pg.glyph_index = info.internal_index;
        out.glyphs.push_back(pg);

        cursor_x += gp.advance.x * scale;
    }

    LayoutLine line{};
    line.first_glyph = first;
    line.glyph_count = static_cast<uint32_t>(out.glyphs.size()) - first;
    line.width = cursor_x;
    out.lines.push_back(line);

    if (cursor_x > out.total_width) {
        out.total_width = cursor_x;
    }
}

void Font::layout_single_line(string_view text, float scale, float ascender_px,
                               float line_height_px, float available_width,
                               TextLayoutResult& out)
{
    vector<GlyphPosition> positions;
    shape_text(text, positions);

    // Ellipsis glyph for truncation.
    bool truncate = available_width > 0.f;
    float ellipsis_advance_px = 0.f;
    GlyphInfo ellipsis_info{};
    if (truncate) {
        vector<GlyphPosition> ellipsis_pos;
        shape_text(u8"\u2026", ellipsis_pos);
        if (!ellipsis_pos.empty()) {
            ellipsis_info = ensure_glyph(ellipsis_pos[0].glyph_id);
            ellipsis_advance_px = ellipsis_pos[0].advance.x * scale;
        }
    }

    float cursor_x = 0.f;
    bool did_truncate = false;

    for (size_t i = 0; i < positions.size(); ++i) {
        auto& gp = positions[i];
        float next_advance = gp.advance.x * scale;

        if (truncate && cursor_x + next_advance + ellipsis_advance_px > available_width) {
            did_truncate = true;
            break;
        }

        GlyphInfo info = ensure_glyph(gp.glyph_id);
        if (info.empty) {
            cursor_x += next_advance;
            continue;
        }

        const float bearing_x_px = info.bbox_min.x * scale;
        const float bearing_y_px = info.bbox_max.y * scale;
        const float glyph_w_px = (info.bbox_max.x - info.bbox_min.x) * scale;
        const float glyph_h_px = (info.bbox_max.y - info.bbox_min.y) * scale;

        PositionedGlyph pg{};
        pg.pos.x = cursor_x + gp.offset.x * scale + bearing_x_px;
        pg.pos.y = ascender_px - bearing_y_px + gp.offset.y * scale;
        pg.size = {glyph_w_px, glyph_h_px};
        pg.glyph_index = info.internal_index;
        out.glyphs.push_back(pg);

        cursor_x += next_advance;
    }

    if (did_truncate && !ellipsis_info.empty) {
        const float bearing_x_px = ellipsis_info.bbox_min.x * scale;
        const float bearing_y_px = ellipsis_info.bbox_max.y * scale;
        const float glyph_w_px = (ellipsis_info.bbox_max.x - ellipsis_info.bbox_min.x) * scale;
        const float glyph_h_px = (ellipsis_info.bbox_max.y - ellipsis_info.bbox_min.y) * scale;

        PositionedGlyph pg{};
        pg.pos.x = cursor_x + bearing_x_px;
        pg.pos.y = ascender_px - bearing_y_px;
        pg.size = {glyph_w_px, glyph_h_px};
        pg.glyph_index = ellipsis_info.internal_index;
        out.glyphs.push_back(pg);

        cursor_x += ellipsis_advance_px;
    }

    LayoutLine line{};
    line.first_glyph = 0;
    line.glyph_count = static_cast<uint32_t>(out.glyphs.size());
    line.width = cursor_x;
    out.lines.push_back(line);

    out.total_width = cursor_x;
    out.total_height = line_height_px;
}

void Font::layout_multi_line(string_view text, float scale, float ascender_px,
                              float line_height_px, TextLayoutResult& out)
{
    float baseline_y = 0.f;
    size_t start = 0;

    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == string_view::npos) ? text.size() : nl;
        string_view line = text.substr(start, end - start);

        layout_line_glyphs(line, scale, ascender_px, baseline_y, out);

        // Empty lines still get a LayoutLine so blank lines have height.
        if (line.empty()) {
            LayoutLine ll{};
            ll.first_glyph = static_cast<uint32_t>(out.glyphs.size());
            ll.glyph_count = 0;
            ll.width = 0.f;
            out.lines.push_back(ll);
        }

        baseline_y += line_height_px;
        start = end + 1;
        if (nl == string_view::npos) {
            break;
        }
    }

    out.total_height = static_cast<float>(out.lines.size()) * line_height_px;
}

void Font::layout_word_wrap(string_view text, float scale, float ascender_px,
                             float line_height_px, float available_width,
                             TextLayoutResult& out)
{
    float baseline_y = 0.f;
    size_t para_start = 0;

    while (para_start <= text.size()) {
        size_t nl = text.find('\n', para_start);
        size_t para_end = (nl == string_view::npos) ? text.size() : nl;
        string_view paragraph = text.substr(para_start, para_end - para_start);

        if (paragraph.empty()) {
            LayoutLine ll{};
            ll.first_glyph = static_cast<uint32_t>(out.glyphs.size());
            ll.glyph_count = 0;
            ll.width = 0.f;
            out.lines.push_back(ll);
            baseline_y += line_height_px;
        } else if (available_width <= 0.f) {
            layout_line_glyphs(paragraph, scale, ascender_px, baseline_y, out);
            baseline_y += line_height_px;
        } else {
            vector<GlyphPosition> positions;
            shape_text(paragraph, positions);

            float line_cursor = 0.f;
            size_t line_start_glyph = 0;
            size_t line_start_char = 0;
            size_t last_space_glyph = 0;
            size_t last_space_char = 0;
            bool has_space = false;

            size_t char_idx = 0;
            for (size_t gi = 0; gi < positions.size(); ++gi) {
                float advance_px = positions[gi].advance.x * scale;

                if (char_idx < paragraph.size()
                    && (paragraph[char_idx] == ' ' || paragraph[char_idx] == '\t')) {
                    last_space_glyph = gi;
                    last_space_char = char_idx;
                    has_space = true;
                }

                if (line_cursor + advance_px > available_width && gi > line_start_glyph) {
                    size_t break_char;
                    if (has_space && last_space_glyph > line_start_glyph) {
                        break_char = last_space_char;
                    } else {
                        break_char = char_idx;
                    }

                    string_view seg = paragraph.substr(line_start_char, break_char - line_start_char);
                    layout_line_glyphs(seg, scale, ascender_px, baseline_y, out);
                    baseline_y += line_height_px;

                    size_t resume_char = break_char;
                    while (resume_char < paragraph.size()
                           && (paragraph[resume_char] == ' ' || paragraph[resume_char] == '\t')) {
                        ++resume_char;
                    }

                    paragraph = paragraph.substr(resume_char);
                    positions.clear();
                    if (!paragraph.empty()) {
                        shape_text(paragraph, positions);
                    }
                    gi = static_cast<size_t>(-1);
                    char_idx = static_cast<size_t>(-1);
                    line_cursor = 0.f;
                    line_start_glyph = 0;
                    line_start_char = 0;
                    has_space = false;
                    continue;
                }

                line_cursor += advance_px;
                ++char_idx;
            }

            if (!paragraph.empty()) {
                layout_line_glyphs(paragraph, scale, ascender_px, baseline_y, out);
                baseline_y += line_height_px;
            }
        }

        para_start = para_end + 1;
        if (nl == string_view::npos) {
            break;
        }
    }

    out.total_height = static_cast<float>(out.lines.size()) * line_height_px;
}

void Font::layout_text(string_view text, float font_size, TextLayout mode,
                        float available_width, TextLayoutResult& out)
{
    out.glyphs.clear();
    out.lines.clear();
    out.total_width = 0.f;
    out.total_height = 0.f;
    out.line_height = 0.f;

    if (text.empty() || !ft_face_) {
        return;
    }

    auto state = read_state<IFont>(this);
    float upem = state ? state->units_per_em : 0.f;
    if (upem <= 0.f || font_size <= 0.f) {
        return;
    }

    const float scale = font_size / upem;
    const float ascender_px = (state ? state->ascender : 0.f) * scale;
    const float line_height_px = (state ? state->line_height : 0.f) * scale;
    out.line_height = line_height_px;

    switch (mode) {
    case TextLayout::SingleLine:
        layout_single_line(text, scale, ascender_px, line_height_px, available_width, out);
        break;
    case TextLayout::MultiLine:
        layout_multi_line(text, scale, ascender_px, line_height_px, out);
        break;
    case TextLayout::WordWrap:
        layout_word_wrap(text, scale, ascender_px, line_height_px, available_width, out);
        break;
    }
}

} // namespace velk::ui::impl
