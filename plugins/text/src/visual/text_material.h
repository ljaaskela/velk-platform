#ifndef VELK_UI_TEXT_TEXT_MATERIAL_H
#define VELK_UI_TEXT_TEXT_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Internal interface for binding a TextMaterial to a font's three
 *        GPU buffers (curves, bands, glyph table).
 */
class ITextMaterialInternal : public Interface<ITextMaterialInternal>
{
public:
    virtual void set_font_buffers(IBuffer::Ptr curves,
                                  IBuffer::Ptr bands,
                                  IBuffer::Ptr glyphs) = 0;
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
    void set_font_buffers(IBuffer::Ptr curves,
                          IBuffer::Ptr bands,
                          IBuffer::Ptr glyphs) override;

    // IMaterial
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;

    string_view get_eval_src() const override;
    string_view get_eval_fn_name() const override;
    string_view get_vertex_src() const override;
    void register_eval_includes(IRenderContext& ctx) const override;

private:
    IBuffer::Ptr curves_;
    IBuffer::Ptr bands_;
    IBuffer::Ptr glyphs_;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_TEXT_MATERIAL_H
