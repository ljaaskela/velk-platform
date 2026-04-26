#ifndef VELK_UI_TEXT_VISUAL_H
#define VELK_UI_TEXT_VISUAL_H

#include <velk/api/change.h>
#include <velk/api/object.h>

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-scene/ext/trait.h>
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
class TextVisual : public ::velk::ext::Visual2D<TextVisual, ITextVisual,
                                         ::velk::IAnalyticShape>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Text, "TextVisual");

    // ITextVisual
    void set_font(const IFont::Ptr& font) override;

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;
    aabb get_local_bounds(const ::velk::size& bounds) const override;
    vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext& ctx) const override;

    // Deferred coverage-discard is no longer needed here: TextMaterial's
    // eval produces alpha-modulated coverage and the deferred fragment
    // driver's discard threshold drops sub-threshold fragments.

    // IAnalyticShape: glyph-shaped shape intersect. Rect-based (kind 0)
    // with a custom intersect that does rect test + slug coverage, so
    // shadow rays hit the glyph silhouette instead of the quad.
    uint32_t get_shape_kind() const override { return 0; }
    string_view get_shape_intersect_source() const override;
    string_view get_shape_intersect_fn_name() const override;
    void register_shape_intersect_includes(::velk::IRenderContext& ctx) const override;

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

    // Populates `layout_result_` lazily based on the current state and
    // `bounds.width`. Returns the font used (nullptr if none available).
    // Shared by get_draw_entries (which emits glyph quads) and
    // get_local_bounds (which reports the laid-out extent to the
    // layout solver). Cached via ChangeCache so repeated calls with
    // identical inputs are free.
    IFont* ensure_layout(const ::velk::size& bounds) const;

    Font font_;
    mutable IFont::TextLayoutResult layout_result_;
    mutable ChangeCache<CacheKey> cache_;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_VISUAL_H
