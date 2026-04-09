#include "env_decoder.h"

#include "environment.h"

#include <velk/ext/core_object.h>
#include <velk/interface/resource/intf_resource.h>

// STB_IMAGE_IMPLEMENTATION is defined in image_decoder.cpp
#include "stb_image.h"

#include <cstdint>

namespace velk::ui::impl {

namespace {

// IEEE 754 float32 to float16 conversion. Handles normals, denormals,
// inf, and NaN. Used to convert stbi_loadf output (32-bit floats) to
// RGBA16F GPU data (16-bit half floats), halving memory usage.
uint16_t float_to_half(float value)
{
    uint32_t f;
    std::memcpy(&f, &value, 4);

    uint32_t sign = (f >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((f >> 23) & 0xFFu) - 127;
    uint32_t mantissa = f & 0x007FFFFFu;

    if (exponent > 15) {
        // Overflow: clamp to half-float max (65504) or inf.
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (exponent < -14) {
        // Denormalized or too small.
        if (exponent < -24) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x00800000u;
        uint32_t shift = static_cast<uint32_t>(-1 - exponent);
        return static_cast<uint16_t>(sign | (mantissa >> (shift + 1)));
    }

    return static_cast<uint16_t>(
        sign |
        (static_cast<uint32_t>(exponent + 15) << 10) |
        (mantissa >> 13));
}

} // namespace

IResource::Ptr EnvDecoder::decode(const IResource::Ptr& inner) const
{
    auto* file = interface_cast<IFile>(inner);
    if (!file) {
        VELK_LOG(E, "EnvDecoder: Resource must implement IFile.");
        return nullptr;
    }

    auto env = instance().create<IEnvironmentInternal>(ClassId::Environment);

    vector<uint8_t> bytes;
    if (!(env && succeeded(file->read(bytes)) && !bytes.empty())) {
        VELK_LOG(E, "EnvDecoder: Failed to read '%s'", file->uri());
        return env;
    }

    int w = 0, h = 0, channels = 0;
    float* decoded = stbi_loadf_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);

    if (decoded) {
        if (w > 0 && h > 0) {
            // Convert float32 RGBA to half-float RGBA16F.
            size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
            size_t half_bytes = pixel_count * 4 * sizeof(uint16_t); // 4 channels, 2 bytes each
            vector<uint8_t> pixels;
            pixels.resize(half_bytes);
            auto* dst = reinterpret_cast<uint16_t*>(pixels.data());
            for (size_t i = 0; i < pixel_count * 4; ++i) {
                dst[i] = float_to_half(decoded[i]);
            }
            env->init(inner->uri(), w, h, std::move(pixels));
        }
        stbi_image_free(decoded);
    }

    return env;
}

} // namespace velk::ui::impl
