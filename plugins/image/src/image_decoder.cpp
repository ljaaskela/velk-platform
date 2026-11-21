#include "image_decoder.h"

#include "image.h"
#include "ktx_decode.h"

#include <velk/api/perf.h>
#include <velk/ext/core_object.h>
#include <velk/interface/resource/intf_resource.h>

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO    // we feed bytes directly, no FILE*
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

namespace velk::ui::impl {

namespace {

/// Result of decoding one image on a worker thread.
struct DecodedPixels
{
    int width{};
    int height{};
    vector<uint8_t> pixels; ///< RGBA8, or for a KTX file its levels in `format`. Empty on failure.
    ImageAverages averages; ///< Worked out here, off the main thread.
    PixelFormat format{PixelFormat::RGBA8_SRGB};
    uint32_t levels{};      ///< Levels of its own (KTX); 0 for a file without.
};

/// Reads @p file and decodes it: a KTX file for a device that samples @p sampled, anything
/// else to RGBA8. Safe to call from any thread.
DecodedPixels read_and_decode(const IFile& file, const vector<PixelFormat>& sampled)
{
    VELK_PERF_SCOPE("image.decode");

    DecodedPixels out;
    vector<uint8_t> bytes;
    if (!succeeded(file.read(bytes)) || bytes.empty()) {
        return out;
    }

    if (is_ktx(bytes.data(), bytes.size())) {
        KtxImage ktx = decode_ktx(bytes.data(), bytes.size(), sampled, file.uri());
        out.width = ktx.width;
        out.height = ktx.height;
        out.pixels = std::move(ktx.data);
        out.averages = ktx.averages;
        out.format = ktx.format;
        out.levels = ktx.levels;
        return out;
    }

    int w = 0, h = 0, channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!decoded || w <= 0 || h <= 0) {
        if (decoded) stbi_image_free(decoded);
        return out;
    }

    // Copy decoded RGBA8 pixels into a velk vector and free stb's buffer.
    // Bulk copy: a per-byte loop here costs seconds across a large scene's
    // texture set. stb's buffer cannot be adopted directly, so one memcpy is
    // the floor. (resize still zero-fills first, so each byte is written
    // twice; removing that needs an uninitialized resize on velk::vector.)
    size_t byte_count = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    out.pixels.resize(byte_count);
    std::memcpy(out.pixels.data(), decoded, byte_count);
    stbi_image_free(decoded);
    out.width = w;
    out.height = h;
    out.averages = average_rgba8(out.pixels.data(), static_cast<size_t>(w) * static_cast<size_t>(h));
    return out;
}

} // namespace

ImageDecoder::ImageDecoder()
    : commit_pool_(create_manual_task_pool()),
      decode_pool_(create_threaded_task_pool())
{
    // Until told what the device samples: the block formats such devices commonly do.
#if defined(__ANDROID__) || defined(__APPLE__) && defined(__arm64__)
    sampled_ = {PixelFormat::ASTC_4x4, PixelFormat::ASTC_4x4_SRGB, PixelFormat::ETC2_RGBA8, PixelFormat::ETC2_RGBA8_SRGB};
#else
    sampled_ = {PixelFormat::BC7_RGBA, PixelFormat::BC7_RGBA_SRGB, PixelFormat::BC1_RGBA, PixelFormat::BC1_RGBA_SRGB,
                PixelFormat::BC3_RGBA, PixelFormat::BC3_RGBA_SRGB, PixelFormat::BC4_R, PixelFormat::BC5_RG};
#endif
}

void ImageDecoder::set_sampled_formats(const vector<PixelFormat>& formats)
{
    sampled_ = formats;
}

IResource::Ptr ImageDecoder::decode(const IResource::Ptr& inner) const
{
    if (!inner) {
        return nullptr;
    }
    auto file = interface_pointer_cast<IFile>(inner);
    if (!file) {
        return nullptr;
    }

    auto obj = ::velk::ext::make_object<Image>();
    if (!obj) {
        return nullptr;
    }
    auto image = interface_pointer_cast<IImage>(obj);
    static_cast<Image*>(obj.get())->begin_loading(inner->uri());

    // The worker only touches the file and its own locals; the image is
    // updated in drain_commits() on the main thread.
    decode_pool_.post([file, image, commits = commit_pool_, sampled = sampled_]() mutable {
        auto result = read_and_decode(*file, sampled);
        commits.post([image, result = std::move(result)]() mutable {
            auto* img = static_cast<Image*>(image.get());
            if (result.pixels.empty()) {
                img->fail_loading();
            } else if (result.levels > 0) {
                img->finish_loading_levels(result.width, result.height, result.format, result.levels,
                                           std::move(result.pixels), result.averages);
            } else {
                img->finish_loading(result.width, result.height, std::move(result.pixels), result.averages);
            }
        });
    });

    return interface_pointer_cast<IResource>(obj);
}

void ImageDecoder::drain_commits()
{
    VELK_PERF_SCOPE("image.drain_commits");
    commit_pool_.drain();
}

} // namespace velk::ui::impl
