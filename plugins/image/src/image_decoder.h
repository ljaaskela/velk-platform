#ifndef VELK_UI_IMAGE_DECODER_H
#define VELK_UI_IMAGE_DECODER_H

#include <velk/ext/object.h>
#include <velk/interface/resource/intf_resource_decoder.h>

#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Resource decoder that turns raw image bytes (png, jpg, bmp, ...)
 *        into `Image` objects.
 *
 * Registered with the resource store under the name "image". Apps fetch
 * decoded images via `instance().resource_store().get_resource<IImage>(
 *     "image:app://path/to/file.png")`.
 */
class ImageDecoder final : public ::velk::ext::Object<ImageDecoder, IResourceDecoder>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::ImageDecoder, "ImageDecoder");

    string_view name() const override { return "image"; }
    IResource::Ptr decode(const IResource::Ptr& inner) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_DECODER_H
