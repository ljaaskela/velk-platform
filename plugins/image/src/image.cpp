#include "image.h"

#include <velk/api/event.h>

#include <cmath>
#include <cstring>

namespace velk::ui::impl {

ImageAverages average_rgba8(const uint8_t* pixels, size_t pixel_count)
{
    ImageAverages out;
    if (!pixels || pixel_count == 0) return out;
    // sRGB values made linear, a byte at a time.
    float linear[256];
    for (int i = 0; i < 256; ++i) {
        const float c = float(i) / 255.f;
        linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    double raw[3] = {};
    double srgb[3] = {};
    double raw_weighted[3] = {};
    double srgb_weighted[3] = {};
    double alpha = 0.0;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t* p = pixels + i * 4u;
        const double a = double(p[3]) / 255.0;
        alpha += a;
        for (int c = 0; c < 3; ++c) {
            const double r = double(p[c]) / 255.0;
            const double s = double(linear[p[c]]);
            raw[c] += r;
            srgb[c] += s;
            raw_weighted[c] += r * a;
            srgb_weighted[c] += s * a;
        }
    }
    const double n = double(pixel_count);
    // Weighted by alpha, so that cut-out texels do not darken it; the plain mean where all
    // texels are cut out.
    const bool weighted = alpha > 0.0;
    float r[3];
    float s[3];
    for (int c = 0; c < 3; ++c) {
        r[c] = float(weighted ? raw_weighted[c] / alpha : raw[c] / n);
        s[c] = float(weighted ? srgb_weighted[c] / alpha : srgb[c] / n);
    }
    out.raw = {r[0], r[1], r[2], float(alpha / n)};
    out.from_srgb = {s[0], s[1], s[2], out.raw.a};
    out.known = true;
    return out;
}

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
    const size_t texels = size_t(width > 0 ? width : 0) * size_t(height > 0 ? height : 0);
    const bool rgba8 = format == PixelFormat::RGBA8 || format == PixelFormat::RGBA8_SRGB;
    averages_ = rgba8 && pixels_.size() == texels * 4u ? average_rgba8(pixels_.data(), texels) : ImageAverages{};
}

color Image::get_average_color() const
{
    if (!averages_.known) return color::white();
    if (format_ == PixelFormat::RGBA8_SRGB) return averages_.from_srgb;
    if (format_ == PixelFormat::RGBA8) return averages_.raw;
    return color::white();
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
    averages_ = {};
}

void Image::begin_loading(string_view uri)
{
    uri_ = string(uri);
    width_ = 0;
    height_ = 0;
    pixels_ = vector<uint8_t>{};
    status_ = ImageStatus::Loading;
    dirty_ = false;
    averages_ = {};
}

void Image::finish_loading(int width, int height, vector<uint8_t> pixels, const ImageAverages& averages)
{
    width_ = width;
    height_ = height;
    pixels_ = std::move(pixels);
    averages_ = averages;
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
    averages_ = {};
    on_loaded().invoke();
}

} // namespace velk::ui::impl
