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
 * Migrated to the eval-driver architecture: one `velk_eval_texture`
 * body produces forward / deferred / RT-fill variants via the shared
 * driver templates.
 */
class TextureMaterial : public ::velk::ext::Material<TextureMaterial, ITextureVisual>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Material::Texture, "TextureMaterial");

    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size, ITextureResolver* resolver = nullptr) const override;

    string_view get_source(string_view role) const override;
    string_view get_fn_name(string_view role) const override;
    /// Its texture's mean colour times its tint.
    color get_average_albedo() const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_TEXTURE_MATERIAL_H
