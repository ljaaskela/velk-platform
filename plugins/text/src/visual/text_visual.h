#ifndef VELK_UI_TEXT_VISUAL_H
#define VELK_UI_TEXT_VISUAL_H

#include "../font_atlas.h"

#include <velk-ui/ext/visual.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/interface/intf_texture_provider.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk_ui {

/**
 * @brief Renders shaped text as textured glyph quads.
 *
 * Uses IFont for text shaping and an internal GlyphAtlas for rasterization.
 * Implements IVisual (draw commands), ITextureProvider (atlas pixels),
 * and ITextVisual (text content setting).
 */
class TextVisual : public ext::Visual<TextVisual, ITextureProvider, ITextVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Text, "TextVisual");

    /**
     * @brief Shapes text with the given font and caches glyph draw commands.
     *
     * Populates the internal glyph atlas and builds TexturedQuad commands.
     * Fires on_visual_changed when done.
     */
    // ITextVisual
    void set_text(velk::string_view text, IFont& font) override;

    // IVisual
    velk::vector<DrawCommand> get_draw_commands(const velk::rect& bounds) override;

    // ITextureProvider
    const uint8_t* get_pixels() const override;
    uint32_t get_texture_width() const override;
    uint32_t get_texture_height() const override;
    bool is_texture_dirty() const override;
    void clear_texture_dirty() override;

private:
    GlyphAtlas atlas_;
    velk::vector<DrawCommand> cached_commands_;
};

} // namespace velk_ui

#endif // VELK_UI_TEXT_VISUAL_H
