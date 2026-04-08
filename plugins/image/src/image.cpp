#include "image.h"

namespace velk::ui::impl {

Image::Image() = default;

void Image::init(string_view uri, int width, int height, PixelFormat format,
                 vector<uint8_t> pixels)
{
    uri_ = string(uri);
    width_ = width;
    height_ = height;
    format_ = format;
    pixels_ = std::move(pixels);
    status_ = ImageStatus::Loaded;
    dirty_ = true; // The renderer will pick this up on next frame.
}

void Image::init_failed(string_view uri)
{
    uri_ = string(uri);
    width_ = 0;
    height_ = 0;
    pixels_ = vector<uint8_t>{};
    status_ = ImageStatus::Failed;
    dirty_ = false;
}

} // namespace velk::ui::impl
