#ifndef VELK_UI_TEXT_TEXT_MATERIAL_H
#define VELK_UI_TEXT_TEXT_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Material for analytic-Bezier text rendering (Slug-style coverage).
 *
 * One TextMaterial instance per font (a TextVisual that switches fonts
 * gets a fresh material). Holds the font's three GPU buffers (curves,
 * bands, glyph table) and emits their GPU virtual addresses as the
 * material's per-draw GPU data. The slug fragment shader binds them via
 * `buffer_reference` and walks them to compute coverage.
 *
 * The pipeline (vertex + fragment shader) is compiled lazily on the first
 * call to `get_pipeline_handle`. The text-specific GLSL include
 * (`velk_text.glsl`, defining the slug coverage function) is registered
 * with the render context just before compilation.
 */
class TextMaterial : public ::velk::ext::Material<TextMaterial>
{
public:
    VELK_CLASS_UID("e0d0f4f6-0c4b-4a8b-b7e4-7e2d6e1a0002", "TextMaterial");

    /// Bind the material to a font's three GPU buffers. Called once after
    /// creation. The buffers' lifetimes are extended for as long as this
    /// material is alive.
    void set_font_buffers(IBuffer::Ptr curves, IBuffer::Ptr bands, IBuffer::Ptr glyphs);

    // IMaterial
    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t gpu_data_size() const override;
    ReturnValue write_gpu_data(void* out, size_t size) const override;

private:
    IBuffer::Ptr curves_;
    IBuffer::Ptr bands_;
    IBuffer::Ptr glyphs_;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_TEXT_MATERIAL_H
