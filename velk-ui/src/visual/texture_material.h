#ifndef VELK_UI_TEXTURE_MATERIAL_H
#define VELK_UI_TEXTURE_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/interface/intf_texture_visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Material that samples a texture and multiplies by a tint color.
 *
 * Used internally by TextureVisual. The texture is bound via the draw entry's
 * texture_key; the renderer resolves it to a bindless index. The tint color
 * is written as material GPU data.
 */
class TextureMaterial : public ::velk::ext::Material<TextureMaterial, ITextureVisual>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Material::Texture, "TextureMaterial");

    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_TEXTURE_MATERIAL_H
