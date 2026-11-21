#include "image_encoder.h"

#include <velk/api/perf.h>
#include <velk/string.h>

#include <cstring>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_surface.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace velk::ui::impl {

namespace {

bool channels_for_format(PixelFormat fmt, int& out_channels)
{
    switch (fmt) {
    case PixelFormat::RGBA8:
    case PixelFormat::RGBA8_SRGB:
        out_channels = 4;
        return true;
    case PixelFormat::R8:
        out_channels = 1;
        return true;
    default:
        return false;
    }
}

struct ImageView
{
    int width = 0;
    int height = 0;
    PixelFormat format{};
    const uint8_t* data = nullptr;
    size_t size = 0;
};

bool view_image(const IImage& image, ImageView& out)
{
    const auto* surface = interface_cast<const ISurface>(&image);
    const auto* buffer  = interface_cast<const IBuffer>(&image);
    if (!surface || !buffer) {
        return false;
    }
    const uvec2 dims = surface->get_dimensions();
    out.width  = static_cast<int>(dims.x);
    out.height = static_cast<int>(dims.y);
    out.format = surface->format();
    out.data   = buffer->get_data();
    out.size   = buffer->get_data_size();
    return true;
}

} // namespace

bool ImageEncoder::save_png(int width, int height, PixelFormat format,
                            const uint8_t* pixels, size_t pixel_size,
                            string_view path) const
{
    VELK_PERF_SCOPE("image.encode_png");

    int channels = 0;
    if (!channels_for_format(format, channels)) {
        return false;
    }
    if (width <= 0 || height <= 0 || !pixels) {
        return false;
    }
    const size_t expected =
        static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
    if (pixel_size < expected) {
        return false;
    }

    string path_str(path);
    const int stride = width * channels;
    return stbi_write_png(path_str.c_str(), width, height, channels, pixels, stride) != 0;
}

bool ImageEncoder::encode_png(int width, int height, PixelFormat format,
                              const uint8_t* pixels, size_t pixel_size,
                              vector<uint8_t>& out) const
{
    VELK_PERF_SCOPE("image.encode_png");

    int channels = 0;
    if (!channels_for_format(format, channels) || width <= 0 || height <= 0 || !pixels) {
        return false;
    }
    const size_t expected =
        static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
    if (pixel_size < expected) {
        return false;
    }
    out.clear();
    const auto append = [](void* context, void* data, int size) {
        auto& bytes = *static_cast<vector<uint8_t>*>(context);
        const size_t at = bytes.size();
        bytes.resize(at + static_cast<size_t>(size));
        std::memcpy(bytes.data() + at, data, static_cast<size_t>(size));
    };
    return stbi_write_png_to_func(append, &out, width, height, channels, pixels, width * channels) != 0;
}

bool ImageEncoder::save_png(const IImage& image, string_view path) const
{
    ImageView v;
    if (!view_image(image, v)) return false;
    return save_png(v.width, v.height, v.format, v.data, v.size, path);
}

bool ImageEncoder::save_hdr(int width, int height, PixelFormat format,
                            const uint8_t* pixels, size_t pixel_size,
                            string_view path) const
{
    VELK_PERF_SCOPE("image.encode_hdr");

    // RGBA32F is supported directly. RGBA16F requires a half->float
    // expansion that we can add when a caller needs it.
    if (format != PixelFormat::RGBA32F) {
        return false;
    }
    if (width <= 0 || height <= 0 || !pixels) {
        return false;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 16u;
    if (pixel_size < expected) {
        return false;
    }

    string path_str(path);
    const float* fdata = reinterpret_cast<const float*>(pixels);
    return stbi_write_hdr(path_str.c_str(), width, height, 4, fdata) != 0;
}

bool ImageEncoder::save_hdr(const IImage& image, string_view path) const
{
    ImageView v;
    if (!view_image(image, v)) return false;
    return save_hdr(v.width, v.height, v.format, v.data, v.size, path);
}

} // namespace velk::ui::impl
