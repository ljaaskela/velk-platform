#ifndef VELK_UI_IMAGE_VISUAL_H
#define VELK_UI_IMAGE_VISUAL_H

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-ui/ext/trait.h>
#include <velk-ui/plugins/image/intf_image_visual.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

class ImageMaterial;

/**
 * @brief Convenience visual that loads an image from a URI and draws it as
 *        a textured quad.
 *
 * On `uri` property change, fetches the image via the resource store
 * (which routes through the `image:` decoder) and binds it to an internal
 * `ImageMaterial`. Apps can either set this directly or use the underlying
 * `ImageMaterial` if they want to attach the same image to a custom visual.
 */
class ImageVisual : public ::velk::ui::ext::Visual<ImageVisual, IImageVisual>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Visual::Image, "ImageVisual");

    ImageVisual();

    vector<DrawEntry> get_draw_entries(const rect& bounds) override;
    vector<IBuffer::Ptr> get_gpu_resources() const override;

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    void ensure_loaded();

    string loaded_uri_;       ///< URI we last fetched (for change detection).
    IImage::Ptr image_;       ///< Decoded image, kept alive by us.
    IObject::Ptr material_;   ///< ImageMaterial instance bound to image_.
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_VISUAL_H
