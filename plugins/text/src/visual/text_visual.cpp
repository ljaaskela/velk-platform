#include "text_visual.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <cstring>

namespace velk::ui {

void TextVisual::set_font(const IFont::Ptr& font)
{
    font_ = font;
    reshape();
    invoke_visual_changed();
}

void TextVisual::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    if (interfaceId == ITextVisual::UID && name == "text") {
        reshape();
    }
    invoke_visual_changed();
}

void TextVisual::ensure_default_font()
{
    if (font_) {
        return;
    }

    auto obj = instance().create<IObject>(ClassId::Font);
    font_ = interface_pointer_cast<IFont>(obj);
    if (font_) {
        font_->init_default();
        font_->set_size(16.f);
    }
}

void TextVisual::reshape()
{
    cached_entries_.clear();
    text_width_ = 0.f;
    text_height_ = 0.f;

    ensure_default_font();
    if (!font_) {
        return;
    }

    auto state = read_state<ITextVisual>(this);
    if (!state || state->text.empty()) {
        return;
    }

    string_view text(state->text.data(), state->text.size());

    vector<IFont::GlyphPosition> positions;
    font_->shape_text(text, positions);

    auto font_state = read_state<IFont>(font_);
    float ascender = font_state ? font_state->ascender : 0.f;
    float line_height = font_state ? font_state->line_height : 0.f;

    float atlas_w = static_cast<float>(atlas_.get_width());
    float atlas_h = static_cast<float>(atlas_.get_height());

    float cursor_x = 0.f;

    for (auto& gp : positions) {
        const AtlasRect* rect = atlas_.ensure_glyph(*font_, gp.glyph_id);
        if (!rect || (rect->w == 0 && rect->h == 0)) {
            cursor_x += gp.advance.x;
            continue;
        }

        float glyph_x = cursor_x + gp.offset.x + rect->bearing_x;
        float glyph_y = ascender - rect->bearing_y + gp.offset.y;
        float glyph_w = static_cast<float>(rect->w);
        float glyph_h = static_cast<float>(rect->h);

        float u0 = static_cast<float>(rect->x) / atlas_w;
        float v0 = static_cast<float>(rect->y) / atlas_h;
        float u1 = static_cast<float>(rect->x + rect->w) / atlas_w;
        float v1 = static_cast<float>(rect->y + rect->h) / atlas_h;

        DrawEntry entry{};
        entry.pipeline_key = PipelineKey::Text;
        entry.bounds = {glyph_x, glyph_y, glyph_w, glyph_h};

        // Pack textured instance data: {x, y, w, h, r, g, b, a, u0, v0, u1, v1}
        // Color is filled at draw time in get_draw_entries; store UVs here.
        float data[] = {glyph_x, glyph_y, glyph_w, glyph_h,
                        0.f, 0.f, 0.f, 0.f,
                        u0, v0, u1, v1};
        std::memcpy(entry.instance_data, data, sizeof(data));
        entry.instance_size = sizeof(data);

        cached_entries_.push_back(entry);
        cursor_x += gp.advance.x;
    }

    text_width_ = cursor_x;
    text_height_ = line_height;
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
        // Offset bounds
        out.bounds.x += offset_x;
        out.bounds.y += offset_y;

        // Update instance data: offset position (first 2 floats) and fill color (floats 4-7)
        float* inst = reinterpret_cast<float*>(out.instance_data);
        inst[0] += offset_x;
        inst[1] += offset_y;
        inst[4] = col.r;
        inst[5] = col.g;
        inst[6] = col.b;
        inst[7] = col.a;

        // Texture key: use this TextureProvider's address as key (matches renderer's upload key)
        out.texture_key = reinterpret_cast<uint64_t>(static_cast<const ITextureProvider*>(this));

        result.push_back(out);
    }

    return result;
}

const uint8_t* TextVisual::get_pixels() const
{
    return atlas_.get_pixels();
}

uint32_t TextVisual::get_texture_width() const
{
    return atlas_.get_width();
}

uint32_t TextVisual::get_texture_height() const
{
    return atlas_.get_height();
}

bool TextVisual::is_texture_dirty() const
{
    return atlas_.is_dirty();
}

void TextVisual::clear_texture_dirty()
{
    atlas_.clear_dirty();
}

} // namespace velk::ui
