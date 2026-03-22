#include "text_visual.h"

#include <velk/api/state.h>

namespace velk_ui {

void TextVisual::set_font(const IFont::Ptr& font)
{
    font_ = font;
    reshape();
    invoke_visual_changed();
}

void TextVisual::on_state_changed(velk::string_view name, velk::IMetadata& owner, velk::Uid interfaceId)
{
    if (interfaceId == ITextVisual::UID) {
        // Only ITextVisual props can affect shaping (?)
        reshape();
    }
    invoke_visual_changed();
}

void TextVisual::reshape()
{
    cached_commands_.clear();

    if (!font_) {
        return;
    }

    auto state = velk::read_state<ITextVisual>(this);
    if (!state || state->text.empty()) {
        return;
    }

    velk::string_view text(state->text.data(), state->text.size());

    velk::vector<IFont::GlyphPosition> positions;
    font_->shape_text(text, positions);

    auto font_state = velk::read_state<IFont>(font_);
    float ascender = font_state ? font_state->ascender : 0.f;

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

        DrawCommand cmd{};
        cmd.type = DrawCommandType::TexturedQuad;
        cmd.bounds = {glyph_x, glyph_y, glyph_w, glyph_h};
        cmd.u0 = static_cast<float>(rect->x) / atlas_w;
        cmd.v0 = static_cast<float>(rect->y) / atlas_h;
        cmd.u1 = static_cast<float>(rect->x + rect->w) / atlas_w;
        cmd.v1 = static_cast<float>(rect->y + rect->h) / atlas_h;

        cached_commands_.push_back(cmd);
        cursor_x += gp.advance.x;
    }
}

velk::vector<DrawCommand> TextVisual::get_draw_commands(const velk::rect& bounds)
{
    auto state = velk::read_state<IVisual>(this);
    velk::color col = state ? state->color : velk::color::white();

    velk::vector<DrawCommand> result;
    result.reserve(cached_commands_.size());

    for (auto& cmd : cached_commands_) {
        DrawCommand out = cmd;
        out.bounds.x += bounds.x;
        out.bounds.y += bounds.y;
        out.color = col;
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

} // namespace velk_ui
