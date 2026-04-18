#ifndef VELK_UI_TEXT_TEXT_MATERIAL_H
#define VELK_UI_TEXT_TEXT_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_snippet.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Internal interface for binding a TextMaterial to a font's three
 *        GPU buffers (curves, bands, glyph table).
 *
 * The Font creates one TextMaterial via the type registry and casts to this
 * interface to wire its own buffers in. Visuals never call this directly:
 * they consume the bound material via `IFont::get_material()`.
 */
class ITextMaterialInternal : public Interface<ITextMaterialInternal>
{
public:
    virtual void set_font_buffers(IBuffer::Ptr curves,
                                  IBuffer::Ptr bands,
                                  IBuffer::Ptr glyphs) = 0;
};

/**
 * @brief Material for analytic-Bezier text rendering (Slug-style coverage).
 *
 * One instance per font, owned by the Font itself. Holds the font's three
 * GPU buffers (curves, bands, glyph table) and emits their GPU virtual
 * addresses as the material's per-draw GPU data. The slug fragment shader
 * binds them via `buffer_reference` and walks them to compute coverage.
 *
 * Because the material is shared across every text visual using the same
 * font, those visuals naturally batch into a single draw call.
 *
 * The pipeline (vertex + fragment shader) is compiled lazily on the first
 * call to `get_pipeline_handle`. The text-specific GLSL include
 * (`velk_text.glsl`, defining the slug coverage function) is registered
 * with the render context just before compilation.
 */
class TextMaterial : public ::velk::ext::Material<TextMaterial, ITextMaterialInternal, ::velk::IShaderSnippet>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::TextMaterial, "TextMaterial");

    // ITextMaterialInternal
    void set_font_buffers(IBuffer::Ptr curves,
                          IBuffer::Ptr bands,
                          IBuffer::Ptr glyphs) override;

    // IMaterial
    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;

    // IShaderSnippet
    string_view get_snippet_fn_name() const override;
    string_view get_snippet_source() const override;
    void register_snippet_includes(IRenderContext& ctx) const override;

private:
    IBuffer::Ptr curves_;
    IBuffer::Ptr bands_;
    IBuffer::Ptr glyphs_;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_TEXT_MATERIAL_H
