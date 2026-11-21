#ifndef VELK_UI_IMAGE_ENCODER_H
#define VELK_UI_IMAGE_ENCODER_H

#include <velk/ext/object.h>
#include <velk-render/interface/intf_image.h>

#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

class ImageEncoder final : public ::velk::ext::Object<ImageEncoder, IImageWriter>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::ImageEncoder, "ImageEncoder");

    bool save_png(const IImage& image, string_view path) const override;
    bool save_png(int width, int height, PixelFormat format,
                  const uint8_t* pixels, size_t pixel_size,
                  string_view path) const override;
    bool encode_png(int width, int height, PixelFormat format,
                    const uint8_t* pixels, size_t pixel_size,
                    vector<uint8_t>& out) const override;
    bool save_hdr(const IImage& image, string_view path) const override;
    bool save_hdr(int width, int height, PixelFormat format,
                  const uint8_t* pixels, size_t pixel_size,
                  string_view path) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_ENCODER_H
