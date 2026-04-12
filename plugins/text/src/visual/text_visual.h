#ifndef VELK_UI_TEXT_VISUAL_H
#define VELK_UI_TEXT_VISUAL_H

#include <velk/api/change.h>
#include <velk/api/object.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Renders shaped text as glyph quads with multiline support.
 *
 * Delegates text layout (shaping, line breaking, word wrapping, ellipsis)
 * to IFont::layout_text(). Converts the positioned glyphs into DrawEntries
 * with alignment and color applied.
 *
 * Layout is performed lazily in get_draw_entries() and cached via
 * ChangeCache so it only re-runs when inputs change.
 */
class TextVisual : public ext::Visual<TextVisual, ITextVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Text, "TextVisual");

    // ITextVisual
    void set_font(const IFont::Ptr& font) override;

    // IVisual
    vector<DrawEntry> get_draw_entries(const rect& bounds) override;
    vector<IBuffer::Ptr> get_gpu_resources() const override;

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    struct CacheKey
    {
        const char* text_data{};
        uint32_t text_size{};
        float font_size{};
        TextLayout layout{};
        float bounds_width{};

        bool operator==(const CacheKey& o) const
        {
            return text_data == o.text_data
                && text_size == o.text_size
                && font_size == o.font_size
                && layout == o.layout
                && bounds_width == o.bounds_width;
        }
        bool operator!=(const CacheKey& o) const { return !(*this == o); }
    };

    void ensure_default_font();
    void bind_font_material();

    Font font_;
    IFont::TextLayoutResult layout_result_;
    ChangeCache<CacheKey> cache_;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_VISUAL_H
