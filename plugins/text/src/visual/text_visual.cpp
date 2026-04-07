#include "text_visual.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-ui/instance_types.h>
#include <velk-ui/plugins/text/intf_text_plugin.h>

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

    auto plugin = get_or_load_plugin<ITextPlugin>(PluginId::TextPlugin);
    if (plugin) {
        font_ = plugin->default_font();
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

    float atlas_w = static_cast<float>(font_->get_atlas_width());
    float atlas_h = static_cast<float>(font_->get_atlas_height());

    float cursor_x = 0.f;

    for (auto& gp : positions) {
        auto* rect = font_->ensure_glyph(gp.glyph_id);
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

        // Color is filled at draw time in get_draw_entries.
        entry.set_instance(TextInstance{
            {glyph_x, glyph_y},
            {glyph_w, glyph_h},
            color::transparent(),
            {u0, v0},
            {u1, v1}});

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

    // Texture key: font's ITexture address (shared across text visuals using the same font)
    auto font_tex = interface_pointer_cast<ITexture>(font_);
    uint64_t tex_key = font_tex ? reinterpret_cast<uint64_t>(font_tex.get()) : 0;

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

        out.texture_key = tex_key;

        result.push_back(out);
    }

    return result;
}

ITexture::Ptr TextVisual::get_texture() const
{
    return interface_pointer_cast<ITexture>(font_);
}

} // namespace velk::ui
