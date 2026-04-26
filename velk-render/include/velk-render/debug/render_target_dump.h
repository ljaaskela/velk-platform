#ifndef VELK_RENDER_DEBUG_RENDER_TARGET_DUMP_H
#define VELK_RENDER_DEBUG_RENDER_TARGET_DUMP_H

#include <velk/api/velk.h>
#include <velk/string.h>
#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_render_backend.h>

#include <cstdint>
#include <cstring>

namespace velk::render::debug {

/**
 * @brief Reads a texture back from the GPU and writes it to disk via @p writer.
 *
 * Picks the file format from the texture's pixel format: 8-bit integer
 * formats go through `IImageWriter::save_png`, RGBA32F goes through
 * `save_hdr`. The chosen extension is appended to @p path_no_ext, so
 * callers don't have to know the format up front.
 *
 * Synchronous — calls `IRenderBackend::read_texture`, which drains the
 * GPU queue. Intended for debug / golden-image dumps; do not call in
 * a hot frame.
 *
 * Header-only because velk_render is a DLL with no exported symbols
 * (see project policy on VELK_EXPORT). The body is small and side-
 * effect free, so inlining at the call site is fine.
 *
 * @return true if both the readback and the file write succeeded.
 */
inline bool dump_texture(IRenderBackend& backend, TextureId texture,
                         const IImageWriter& writer, string_view path_no_ext)
{
    vector<uint8_t> pixels;
    PixelFormat format{};
    uvec2 dims{};
    if (!backend.read_texture(texture, pixels, format, dims)) {
        return false;
    }

    string path(path_no_ext);
    const int w = static_cast<int>(dims.x);
    const int h = static_cast<int>(dims.y);

    switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::RGBA8_SRGB:
    case PixelFormat::R8:
        path += ".png";
        return writer.save_png(w, h, format, pixels.data(), pixels.size(), path);
    case PixelFormat::RGBA32F:
        path += ".hdr";
        return writer.save_hdr(w, h, format, pixels.data(), pixels.size(), path);
    case PixelFormat::RGBA16F:
    default:
        return false;
    }
}

/**
 * @brief Reads an RGBA32F texture back, reinterprets each pixel's bytes
 *        as four `uint32_t` (matching `uintBitsToFloat` writes from a
 *        compute shader), groups distinct tuples, and logs them to
 *        VELK_LOG(I) along with one sample pixel coordinate per tuple.
 *
 * Use this for the RT-shadow `shadow_debug` image (and similar uvec4
 * diagnostics): the .hdr dump is opaque to viewers because the bits
 * don't represent meaningful floats, so this is how we read them back
 * out as integers without writing a separate inspector tool.
 *
 * @p label is prefixed onto every log line.
 * @p max_unique caps the number of tuples printed (sorted by first
 *  appearance). 0 = no cap.
 * @return number of unique tuples observed (capped if exceeded; the
 *  count itself is uncapped).
 */
inline size_t log_unique_uvec4_pixels(IRenderBackend& backend, TextureId texture,
                                      string_view label,
                                      size_t max_unique = 64)
{
    vector<uint8_t> pixels;
    PixelFormat format{};
    uvec2 dims{};
    if (!backend.read_texture(texture, pixels, format, dims)) {
        VELK_LOG(I, "%.*s: read_texture failed", static_cast<int>(label.size()), label.data());
        return 0;
    }
    if (format != PixelFormat::RGBA32F) {
        VELK_LOG(I, "%.*s: format is not RGBA32F (got %d)",
                 static_cast<int>(label.size()), label.data(), static_cast<int>(format));
        return 0;
    }

    const int w = static_cast<int>(dims.x);
    const int h = static_cast<int>(dims.y);
    const size_t pix_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (pixels.size() < pix_count * 16u) {
        VELK_LOG(I, "%.*s: short pixel buffer", static_cast<int>(label.size()), label.data());
        return 0;
    }

    struct Tuple { uint32_t v[4]; int x; int y; };
    vector<Tuple> uniques;
    uniques.reserve(64);

    auto same = [](const uint32_t* a, const uint32_t* b) {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
    };

    size_t total_unique = 0;
    const uint32_t* base = reinterpret_cast<const uint32_t*>(pixels.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t* p = base + (static_cast<size_t>(y) * w + x) * 4;
            bool found = false;
            for (auto& t : uniques) {
                if (same(t.v, p)) { found = true; break; }
            }
            if (found) continue;
            ++total_unique;
            if (max_unique == 0 || uniques.size() < max_unique) {
                Tuple t;
                t.v[0] = p[0]; t.v[1] = p[1]; t.v[2] = p[2]; t.v[3] = p[3];
                t.x = x; t.y = y;
                uniques.push_back(t);
            }
        }
    }

    VELK_LOG(I, "%.*s: %zu unique tuple(s) over %dx%d (showing %zu)",
             static_cast<int>(label.size()), label.data(),
             total_unique, w, h, uniques.size());
    for (size_t i = 0; i < uniques.size(); ++i) {
        const auto& t = uniques[i];
        VELK_LOG(I, "%.*s [%zu] @(%d,%d): r=0x%08x g=0x%08x b=0x%08x a=0x%08x",
                 static_cast<int>(label.size()), label.data(), i, t.x, t.y,
                 t.v[0], t.v[1], t.v[2], t.v[3]);
    }

    return total_unique;
}

} // namespace velk::render::debug

#endif // VELK_RENDER_DEBUG_RENDER_TARGET_DUMP_H
