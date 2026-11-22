#ifndef VELK_UI_TEXT_VISUAL_H
#define VELK_UI_TEXT_VISUAL_H

#include <velk/api/change.h>
#include <velk/api/object.h>

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/interface/intf_content_size.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/** @brief Helpers shared by TextVisual and TextField. */
namespace text_detail {

/** @brief The font @p ref names, or the default font when it is empty or failed to load. */
IFont::Ptr resolve_font(const ObjectRef& ref);

/** @brief Writes @p font's material to @p visual's paint. */
void bind_font_material(IVisual2D* visual, const IFont::Ptr& font);

/**
 * @brief Appends one draw entry per laid-out glyph, aligned in @p bounds and moved by
 *        @p shift_x, @p shift_y.
 */
void append_glyphs(const IFont::TextLayoutResult& layout, const ::velk::size& bounds, HAlign ha, VAlign va,
                   ::velk::color col, const IProgram::Ptr& material, float shift_x, float shift_y,
                   vector<DrawEntry>& out);

/** @brief The glyph-silhouette shape intersect snippet, for IShaderSource kIntersect. */
string_view intersect_source();
/** @brief The function name of intersect_source(). */
string_view intersect_fn_name();

} // namespace text_detail

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
                                         ::velk::IAnalyticShape, IContentSize>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Text, "TextVisual");

    // IContentSize: the laid-out text, wrapped at the available width in WordWrap.
    size content_size(const size& available) override;

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;
    aabb get_local_bounds(const ::velk::size& bounds) const override;
    vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext& ctx) const override;

    // Deferred coverage-discard is no longer needed here: TextMaterial's
    // eval produces alpha-modulated coverage and the deferred fragment
    // driver's discard threshold drops sub-threshold fragments.

    // IAnalyticShape: glyph-shaped shape intersect. Rect-based (kind 0)
    // with a custom intersect (registered via IShaderSource under
    // shader_role::kIntersect) that does rect test + slug coverage,
    // so shadow rays hit the glyph silhouette instead of the quad.
    uint32_t get_shape_kind() const override { return 0; }

    // IShaderSource: contributes the kIntersect snippet.
    string_view get_source(::velk::string_view role) const override;
    string_view get_fn_name(::velk::string_view role) const override;
    void register_includes(::velk::IRenderContext& ctx) const override;

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

    /// Resolves the font to use from the `font` property, falling back to
    /// the default font when it is empty or failed to load.
    void update_font();

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
