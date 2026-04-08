#include "text_visual.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_material.h>
#include <velk-ui/instance_types.h>
#include <velk-ui/plugins/text/intf_text_plugin.h>

namespace velk::ui {

void TextVisual::set_font(const IFont::Ptr& font)
{
    font_ = Font(font);
    bind_font_material();
    reshape();
    invoke_visual_changed();
}

void TextVisual::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    if (interfaceId == ITextVisual::UID && (name == "text" || name == "font_size")) {
        reshape();
    }
    invoke_visual_changed();
}

void TextVisual::ensure_default_font()
{
    if (font_) {
        return;
    }
    font_ = get_default_font();
    if (font_) {
        bind_font_material();
    }
}

void TextVisual::bind_font_material()
{
    // The Font owns one TextMaterial bound to its three GPU buffers and
    // every text visual using the same font shares that single instance.
    // The renderer batches by material identity, so all text visuals on
    // one font collapse into a single draw call.
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return;
    }
    auto material = font->get_material();
    if (!material) {
        return;
    }
    write_state<IVisual>(this, [&](IVisual::State& s) {
        set_object_ref(s.paint, material);
    });
}

void TextVisual::reshape()
{
    cached_entries_.clear();
    text_width_ = 0.f;
    text_height_ = 0.f;

    ensure_default_font();
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return;
    }

    auto state = read_state<ITextVisual>(this);
    if (!state || state->text.empty()) {
        return;
    }

    string_view text(state->text.data(), state->text.size());

    vector<IFont::GlyphPosition> positions;
    font->shape_text(text, positions);

    auto font_state = read_state<IFont>(font);
    float ascender_units    = font_state ? font_state->ascender    : 0.f;
    float line_height_units = font_state ? font_state->line_height : 0.f;
    float upem              = font_state ? font_state->units_per_em : 0.f;
    float size_px           = state->font_size;

    if (upem <= 0.f || size_px <= 0.f) {
        return;
    }
    // Single scale factor converts every font-unit measurement (advances,
    // bbox extents, ascender, line height, gp.offset) to pixels.
    const float scale = size_px / upem;
    const float ascender_px = ascender_units * scale;

    float cursor_x = 0.f;

    for (auto& gp : positions) {
        IFont::GlyphInfo info = font->ensure_glyph(gp.glyph_id);
        if (info.empty) {
            cursor_x += gp.advance.x * scale;
            continue;
        }

        // Convert font-unit bbox to pixel-space quad position and size.
        // bbox_min.x acts as the left side bearing; bbox_max.y is the
        // height from the baseline to the top of the glyph (Y-up).
        const float bearing_x_px = info.bbox_min.x * scale;
        const float bearing_y_px = info.bbox_max.y * scale;
        const float glyph_w_px = (info.bbox_max.x - info.bbox_min.x) * scale;
        const float glyph_h_px = (info.bbox_max.y - info.bbox_min.y) * scale;

        const float glyph_x = cursor_x + gp.offset.x * scale + bearing_x_px;
        const float glyph_y = ascender_px - bearing_y_px + gp.offset.y * scale;

        DrawEntry entry{};
        entry.pipeline_key = 0; // material override supplies the pipeline
        entry.bounds = {glyph_x, glyph_y, glyph_w_px, glyph_h_px};

        // Color is filled at draw time in get_draw_entries.
        TextInstance inst{};
        inst.pos = {glyph_x, glyph_y};
        inst.size = {glyph_w_px, glyph_h_px};
        inst.col = color::transparent();
        inst.glyph_index = info.internal_index;
        entry.set_instance(inst);

        cached_entries_.push_back(entry);
        cursor_x += gp.advance.x * scale;
    }

    text_width_ = cursor_x;
    text_height_ = line_height_units * scale;
}

vector<DrawEntry> TextVisual::get_draw_entries(const rect& bounds)
{
    auto visual_state = read_state<IVisual>(this);
    ::velk::color col = visual_state ? visual_state->color : ::velk::color::white();

    auto text_state = read_state<ITextVisual>(this);
    HAlign ha = text_state ? text_state->h_align : HAlign::Left;
    VAlign va = text_state ? text_state->v_align : VAlign::Top;

    float offset_x = bounds.x;
    float offset_y = bounds.y;

    switch (ha) {
    case HAlign::Center: offset_x += (bounds.width - text_width_) * 0.5f; break;
    case HAlign::Right:  offset_x += bounds.width - text_width_; break;
    default: break;
    }

    switch (va) {
    case VAlign::Center: offset_y += (bounds.height - text_height_) * 0.5f; break;
    case VAlign::Bottom: offset_y += bounds.height - text_height_; break;
    default: break;
    }

    vector<DrawEntry> result;
    result.reserve(cached_entries_.size());

    for (auto& entry : cached_entries_) {
        DrawEntry out = entry;
        out.bounds.x += offset_x;
        out.bounds.y += offset_y;

        auto& inst = out.as_instance<TextInstance>();
        inst.pos.x += offset_x;
        inst.pos.y += offset_y;
        inst.col = col;

        result.push_back(out);
    }

    return result;
}

vector<IBuffer::Ptr> TextVisual::get_gpu_resources() const
{
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return {};
    }
    vector<IBuffer::Ptr> out;
    if (auto b = font->get_curve_buffer()) {
        out.push_back(std::move(b));
    }
    if (auto b = font->get_band_buffer()) {
        out.push_back(std::move(b));
    }
    if (auto b = font->get_glyph_buffer()) {
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace velk::ui
