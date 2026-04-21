#ifndef VELK_UI_IMAGE_MATERIAL_H
#define VELK_UI_IMAGE_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/plugins/image/intf_image_material.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Material that samples a bound `ISurface` and multiplies by a tint.
 *
 * Migrated to the eval-driver architecture: one `velk_eval_image`
 * body produces forward / deferred / RT-fill variants. The texture is
 * bound via the `texture` property on the state; the renderer
 * resolves the draw-call's `texture_key` to a bindless index and
 * exposes it as `root.texture_id` in the fragment driver.
 */
class ImageMaterial : public ::velk::ext::Material<ImageMaterial, IImageMaterial>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Material::Image, "ImageMaterial");

    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size, ITextureResolver* resolver = nullptr) const override;

    string_view get_eval_src() const override;
    string_view get_eval_fn_name() const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_MATERIAL_H
