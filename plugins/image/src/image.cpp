#include "image.h"

#include <velk/api/event.h>

#include <cstring>

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

void Image::init_from_pixels(string_view uri, int width, int height, PixelFormat format,
                              const uint8_t* pixels, size_t pixel_size)
{
    vector<uint8_t> v;
    v.resize(pixel_size);
    if (pixels && pixel_size > 0) {
        std::memcpy(v.data(), pixels, pixel_size);
    }
    init(uri, width, height, format, std::move(v));
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

void Image::begin_loading(string_view uri)
{
    uri_ = string(uri);
    width_ = 0;
    height_ = 0;
    pixels_ = vector<uint8_t>{};
    status_ = ImageStatus::Loading;
    dirty_ = false;
}

void Image::finish_loading(int width, int height, vector<uint8_t> pixels)
{
    width_ = width;
    height_ = height;
    pixels_ = std::move(pixels);
    status_ = ImageStatus::Loaded;
    dirty_ = true; // The renderer will pick this up on next frame.
    on_loaded().invoke();
}

void Image::fail_loading()
{
    width_ = 0;
    height_ = 0;
    pixels_ = vector<uint8_t>{};
    status_ = ImageStatus::Failed;
    dirty_ = false;
    on_loaded().invoke();
}

} // namespace velk::ui::impl
