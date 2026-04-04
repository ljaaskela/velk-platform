#ifndef VELK_UI_TEXT_VISUAL_H
#define VELK_UI_TEXT_VISUAL_H

#include "../font_atlas.h"

#include <velk-ui/ext/visual.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-render/interface/intf_texture_provider.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Renders shaped text as textured glyph quads.
 *
 * Owns the font and text content (via ITextVisual::text PROP).
 * Uses IFont for text shaping and an internal GlyphAtlas for rasterization.
 * Reshapes automatically when text or font changes.
 */
class TextVisual : public ext::Visual<TextVisual, ITextureProvider, ITextVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Text, "TextVisual");

    // ITextVisual
    void set_font(const IFont::Ptr& font) override;

    // IVisual
    vector<DrawEntry> get_draw_entries(const rect& bounds) override;

    // ITextureProvider
    const uint8_t* get_pixels() const override;
    uint32_t get_texture_width() const override;
    uint32_t get_texture_height() const override;
    bool is_texture_dirty() const override;
    void clear_texture_dirty() override;

protected:
    // Override to reshape when the text property changes
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    void reshape();
    void ensure_default_font();

    IFont::Ptr font_;
    GlyphAtlas atlas_;
    vector<DrawEntry> cached_entries_;
    float text_width_{};
    float text_height_{};
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_VISUAL_H
