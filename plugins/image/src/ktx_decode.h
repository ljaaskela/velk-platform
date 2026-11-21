#ifndef VELK_UI_KTX_DECODE_H
#define VELK_UI_KTX_DECODE_H

#include "image.h"

#include <velk/string_view.h>
#include <velk/vector.h>
#include <velk-render/render_types.h>

#include <cstddef>
#include <cstdint>

namespace velk::ui::impl {

/// A KTX file decoded for upload: its levels, largest first, each tightly packed in `format`.
struct KtxImage
{
    int width{};
    int height{};
    PixelFormat format{PixelFormat::RGBA8_SRGB};
    uint32_t levels{};
    vector<uint8_t> data;   ///< Empty on failure.
    ImageAverages averages; ///< Known where the texels could be read.
};

/// Whether @p bytes start as a KTX1 or KTX2 file.
bool is_ktx(const uint8_t* bytes, size_t size);

/**
 * @brief Decodes the KTX1 or KTX2 file in @p bytes, for a device that samples the formats in
 *        @p sampled (block compressed ones; uncompressed ones are taken as sampled).
 *
 * A Basis Universal file is transcoded to the first of BC7, ASTC 4 by 4 and ETC2 the device
 * samples, else to RGBA8. An ASTC or ETC file the device cannot sample is decoded to RGBA8.
 * Uncompressed layouts without a format of their own (RGB, BGR, two channels, half or single
 * floats of fewer channels) are widened to RGBA8, RGBA16F or RGBA32F. The levels are those the
 * file holds. Of an array, a cube map or a 3D texture, the first 2D image. Safe to call from
 * any thread. @p uri only names the file in the log.
 */
KtxImage decode_ktx(const uint8_t* bytes, size_t size, const vector<PixelFormat>& sampled, string_view uri);

} // namespace velk::ui::impl

#endif // VELK_UI_KTX_DECODE_H
