#include "image_decoder.h"

#include "image.h"

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
    vector<uint8_t> pixels; ///< RGBA8. Empty on failure.
};

/// Reads @p file and decodes it to RGBA8. Safe to call from any thread.
DecodedPixels read_and_decode(const IFile& file)
{
    VELK_PERF_SCOPE("image.decode");

    DecodedPixels out;
    vector<uint8_t> bytes;
    if (!succeeded(file.read(bytes)) || bytes.empty()) {
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
    return out;
}

} // namespace

ImageDecoder::ImageDecoder()
    : commit_pool_(create_manual_task_pool()),
      decode_pool_(create_threaded_task_pool())
{
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
    decode_pool_.post([file, image, commits = commit_pool_]() mutable {
        auto result = read_and_decode(*file);
        commits.post([image, result = std::move(result)]() mutable {
            auto* img = static_cast<Image*>(image.get());
            if (result.pixels.empty()) {
                img->fail_loading();
            } else {
                img->finish_loading(result.width, result.height, std::move(result.pixels));
            }
        });
    });

    return interface_pointer_cast<IResource>(obj);
}

void ImageDecoder::drain_commits()
{
    commit_pool_.drain();
}

} // namespace velk::ui::impl
