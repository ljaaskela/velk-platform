#include "image_decoder.h"

#include "image.h"

#include <velk/ext/core_object.h>
#include <velk/interface/resource/intf_resource.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO    // we feed bytes directly, no FILE*
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

namespace velk::ui::impl {

IResource::Ptr ImageDecoder::decode(const IResource::Ptr& inner) const
{
    if (!inner) {
        return nullptr;
    }
    auto* file = interface_cast<IFile>(inner);
    if (!file) {
        return nullptr;
    }

    vector<uint8_t> bytes;
    if (!succeeded(file->read(bytes)) || bytes.empty()) {
        // Inner resource exists but is unreadable. Cache a Failed image so
        // repeated lookups don't keep trying.
        auto obj = ::velk::ext::make_object<Image>();
        if (!obj) {
            return nullptr;
        }
        auto* img = static_cast<Image*>(obj.get());
        img->init_failed(inner->uri());
        return interface_pointer_cast<IResource>(obj);
    }

    int w = 0, h = 0, channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);

    auto obj = ::velk::ext::make_object<Image>();
    if (!obj) {
        if (decoded) stbi_image_free(decoded);
        return nullptr;
    }
    auto* img = static_cast<Image*>(obj.get());

    if (!decoded || w <= 0 || h <= 0) {
        img->init_failed(inner->uri());
        if (decoded) stbi_image_free(decoded);
        return interface_pointer_cast<IResource>(obj);
    }

    // Copy decoded RGBA8 pixels into a velk vector and free stb's buffer.
    size_t byte_count = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    vector<uint8_t> pixels;
    pixels.resize(byte_count);
    for (size_t i = 0; i < byte_count; ++i) {
        pixels[i] = decoded[i];
    }
    stbi_image_free(decoded);

    img->init(inner->uri(), w, h, PixelFormat::RGBA8_SRGB, std::move(pixels));
    return interface_pointer_cast<IResource>(obj);
}

} // namespace velk::ui::impl
