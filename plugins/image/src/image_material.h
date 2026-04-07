#ifndef VELK_UI_IMAGE_MATERIAL_H
#define VELK_UI_IMAGE_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/plugins/image/intf_image_material.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Material that samples a bound `ITexture` and multiplies by a tint.
 *
 * The texture is bound via the `texture` property as an `ObjectRef`. The
 * draw call's `texture_key` is set by `ImageVisual` to the texture pointer
 * (cast to uint64_t); the renderer resolves this through its texture map
 * and writes the resulting bindless index into `DrawDataHeader.texture_id`,
 * which the fragment shader reads.
 */
class ImageMaterial : public ::velk::ext::Material<ImageMaterial, IImageMaterial>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Material::Image, "ImageMaterial");

    uint64_t get_pipeline_handle(IRenderContext& ctx) override;
    size_t gpu_data_size() const override;
    ReturnValue write_gpu_data(void* out, size_t size) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_MATERIAL_H
