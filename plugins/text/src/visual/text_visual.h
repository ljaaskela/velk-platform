#ifndef VELK_UI_TEXT_VISUAL_H
#define VELK_UI_TEXT_VISUAL_H

#include <velk/api/object.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Renders shaped text as textured glyph quads.
 *
 * Holds a reference to an IFont which owns the glyph atlas.
 * Multiple TextVisuals sharing the same font share one atlas,
 * enabling draw call batching across text elements.
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
    void reshape();
    void ensure_default_font();
    void bind_font_material();

    Font font_;
    vector<DrawEntry> cached_entries_;
    float text_width_{};
    float text_height_{};
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_VISUAL_H
