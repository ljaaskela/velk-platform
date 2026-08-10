#ifndef VELK_UI_TEXT_TEXT_MATERIAL_H
#define VELK_UI_TEXT_TEXT_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Internal interface for binding a TextMaterial to the font whose
 *        glyph data it reads.
 */
class ITextMaterialInternal : public Interface<ITextMaterialInternal>
{
public:
    virtual void set_font(IFont* font) = 0;
};

/**
 * @brief Analytic-Bezier text material (Slug-style coverage).
 *
 * Migrated to the eval-driver architecture. One `velk_eval_text` body
 * computes glyph coverage and returns the alpha-modulated color; the
 * framework generates forward / deferred / RT-fill variants. Owns a
 * custom vertex shader because the instance layout carries a per-glyph
 * `glyph_index` that surfaces as the canonical `v_shape_param`.
 */
class TextMaterial : public ::velk::ext::Material<TextMaterial,
                                                   ITextMaterialInternal>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::TextMaterial, "TextMaterial");

    // ITextMaterialInternal
    void set_font(IFont* font) override;

    // IMaterial
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size, ITextureResolver* resolver = nullptr) const override;

    string_view get_source(string_view role) const override;
    string_view get_fn_name(string_view role) const override;
    void register_includes(IRenderContext& ctx) const override;

private:
    using Base = ::velk::ext::Material<TextMaterial, ITextMaterialInternal>;

    IFont* font_ = nullptr;  ///< Owns this material; outlives it.
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_TEXT_MATERIAL_H
