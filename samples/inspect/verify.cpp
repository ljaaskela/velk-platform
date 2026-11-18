#include "verify.h"

#include <velk-render/interface/intf_gpu_resource.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace inspect {

namespace {

float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? 0.f : 65504.f;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

float luminance(const float* rgb)
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

} // namespace

bool grab_window(velk::IRenderBackend& backend, uint64_t surface_id, Image& out)
{
    // Already acquired by the frame just presented, so this does not clear it.
    auto target = backend.acquire_swapchain_texture(surface_id);
    auto* texture = interface_cast<velk::IGpuTexture>(target.get());
    velk::vector<uint8_t> pixels;
    velk::PixelFormat format{};
    velk::uvec2 dims{};
    if (!texture || !backend.read_texture(*texture, pixels, format, dims)) {
        return false;
    }
    const size_t count = static_cast<size_t>(dims.x) * dims.y;
    out.width = dims.x;
    out.height = dims.y;
    out.rgb.resize(count * 3);
    if (format == velk::PixelFormat::RGBA16F) {
        // What the window shows: the composite's linear values, as the display takes them.
        const auto* source = reinterpret_cast<const uint16_t*>(pixels.data());
        for (size_t i = 0; i < count; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                out.rgb[i * 3 + c] = std::pow(std::clamp(half_to_float(source[i * 4 + c]), 0.f, 1.f), 1.f / 2.2f);
            }
        }
        return true;
    }
    if (format == velk::PixelFormat::RGBA8 || format == velk::PixelFormat::RGBA8_SRGB) {
        for (size_t i = 0; i < count; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                out.rgb[i * 3 + c] = static_cast<float>(pixels[i * 4 + c]) / 255.f;
            }
        }
        return true;
    }
    return false;
}

Image downscale(const Image& image, uint32_t factor)
{
    Image out;
    out.width = image.width / factor;
    out.height = image.height / factor;
    out.rgb.resize(size_t(out.width) * out.height * 3, 0.f);
    const float weight = 1.f / static_cast<float>(factor * factor);
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            float* to = out.rgb.data() + (size_t(y) * out.width + x) * 3;
            for (uint32_t j = 0; j < factor; ++j) {
                for (uint32_t i = 0; i < factor; ++i) {
                    const float* from = image.rgb.data() + ((size_t(y) * factor + j) * image.width + size_t(x) * factor + i) * 3;
                    for (int c = 0; c < 3; ++c) to[c] += from[c] * weight;
                }
            }
        }
    }
    return out;
}

Difference compare(const Image& ours, const Image& reference, uint32_t tile)
{
    Difference d;
    if (ours.width != reference.width || ours.height != reference.height || ours.rgb.empty() || tile == 0) {
        d.mean = d.p95 = d.worst_tile = 1.f;
        return d;
    }
    const size_t count = size_t(ours.width) * ours.height;
    velk::vector<float> diffs;
    diffs.resize(count);
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        diffs[i] = std::abs(luminance(ours.rgb.data() + i * 3) - luminance(reference.rgb.data() + i * 3));
        sum += diffs[i];
    }
    // The squares, the last row and column as much as is left of them.
    for (uint32_t ty = 0; ty * tile < ours.height; ++ty) {
        for (uint32_t tx = 0; tx * tile < ours.width; ++tx) {
            double tile_sum = 0.0;
            uint32_t n = 0;
            for (uint32_t y = ty * tile; y < std::min(ours.height, (ty + 1) * tile); ++y) {
                for (uint32_t x = tx * tile; x < std::min(ours.width, (tx + 1) * tile); ++x) {
                    tile_sum += diffs[size_t(y) * ours.width + x];
                    ++n;
                }
            }
            const float mean = n ? static_cast<float>(tile_sum / n) : 0.f;
            if (mean > d.worst_tile) {
                d.worst_tile = mean;
                d.tile_x = tx;
                d.tile_y = ty;
            }
        }
    }
    std::sort(diffs.begin(), diffs.end());
    d.mean = static_cast<float>(sum / static_cast<double>(count));
    d.p95 = diffs[std::min(count - 1, count * 95 / 100)];
    return d;
}

Change frame_change(const Image& before, const Image& after, float threshold)
{
    Change c;
    if (before.width != after.width || before.height != after.height || before.rgb.empty()) {
        return c;
    }
    const size_t count = size_t(before.width) * before.height;
    double sum = 0.0;
    size_t over = 0;
    for (size_t i = 0; i < count; ++i) {
        const float d = std::abs(luminance(after.rgb.data() + i * 3) - luminance(before.rgb.data() + i * 3));
        sum += d;
        over += d > threshold ? 1u : 0u;
    }
    c.mean = static_cast<float>(sum / static_cast<double>(count));
    c.share = static_cast<float>(static_cast<double>(over) / static_cast<double>(count));
    return c;
}

Image difference_map(const Image& ours, const Image& reference)
{
    Image out;
    if (ours.width != reference.width || ours.height != reference.height) {
        return out;
    }
    out.width = ours.width;
    out.height = ours.height;
    out.rgb.resize(ours.rgb.size());
    const size_t count = size_t(ours.width) * ours.height;
    for (size_t i = 0; i < count; ++i) {
        // Four times the difference, so that a tenth reads as clearly coloured.
        const float d = std::clamp((luminance(ours.rgb.data() + i * 3) - luminance(reference.rgb.data() + i * 3)) * 4.f, -1.f, 1.f);
        const float base = luminance(reference.rgb.data() + i * 3) * 0.25f;
        out.rgb[i * 3 + 0] = std::clamp(base + std::max(d, 0.f), 0.f, 1.f);
        out.rgb[i * 3 + 1] = base;
        out.rgb[i * 3 + 2] = std::clamp(base + std::max(-d, 0.f), 0.f, 1.f);
    }
    return out;
}

namespace {

constexpr uint32_t kFloatReference = 0x46455256u;  // "VREF": a float a value
constexpr uint32_t kByteReference = 0x38465256u;   // "VRF8": a byte a value

} // namespace

bool save_reference(const Image& image, velk::string_view path)
{
    const velk::string file(path);
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) {
        return false;
    }
    // A byte a value: finer than any difference the checks look for, at a quarter of the size.
    const uint32_t header[3] = {kByteReference, image.width, image.height};
    velk::vector<uint8_t> bytes;
    bytes.resize(image.rgb.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(std::clamp(image.rgb[i], 0.f, 1.f) * 255.f + 0.5f);
    }
    bool ok = std::fwrite(header, sizeof(header), 1, f) == 1;
    ok = ok && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

bool load_reference(velk::string_view path, Image& out)
{
    const velk::string file(path);
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint32_t header[3] = {};
    bool ok = std::fread(header, sizeof(header), 1, f) == 1 && (header[0] == kFloatReference || header[0] == kByteReference)
           && header[1] < 16384u && header[2] < 16384u;
    if (ok) {
        out.width = header[1];
        out.height = header[2];
        out.rgb.resize(size_t(out.width) * out.height * 3);
        if (header[0] == kFloatReference) {
            ok = std::fread(out.rgb.data(), sizeof(float), out.rgb.size(), f) == out.rgb.size();
        } else {
            velk::vector<uint8_t> bytes;
            bytes.resize(out.rgb.size());
            ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
            for (size_t i = 0; ok && i < bytes.size(); ++i) out.rgb[i] = static_cast<float>(bytes[i]) / 255.f;
        }
    }
    std::fclose(f);
    return ok;
}

bool save_png(const velk::IImageWriter& writer, const Image& image, velk::string_view path)
{
    const size_t count = size_t(image.width) * image.height;
    velk::vector<uint8_t> pixels;
    pixels.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            pixels[i * 4 + c] = static_cast<uint8_t>(std::clamp(image.rgb[i * 3 + c], 0.f, 1.f) * 255.f + 0.5f);
        }
        pixels[i * 4 + 3] = 255;
    }
    return writer.save_png(static_cast<int>(image.width), static_cast<int>(image.height), velk::PixelFormat::RGBA8,
                           pixels.data(), pixels.size(), path);
}

} // namespace inspect
