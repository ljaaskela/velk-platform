#include "env_decoder.h"

#include "environment.h"

#include <velk/api/perf.h>
#include <velk/ext/core_object.h>
#include <velk/interface/resource/intf_resource.h>

#include <velk-render/api/half_float.h>

// STB_IMAGE_IMPLEMENTATION is defined in image_decoder.cpp
#include "stb_image.h"

#include <cstdint>

namespace velk::ui::impl {

IResource::Ptr EnvDecoder::decode(const IResource::Ptr& inner) const
{
    auto* file = interface_cast<IFile>(inner);
    if (!file) {
        VELK_LOG(E, "EnvDecoder: Resource must implement IFile.");
        return nullptr;
    }

    VELK_PERF_SCOPE("image.env_decode");

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
            size_t total_floats = pixel_count * 4;

            // Sanitize radiance to the finite half-float range before
            // conversion. A bright HDR sun can exceed the RGBA16F max
            // (65504) and convert to +Inf; linear mip downsampling then
            // spreads Inf and produces NaN, blacking out the averaged mips
            // that env diffuse / rough specular sample. Clamp over-range to
            // the max finite half and map NaN to zero so the whole chain
            // stays finite.
            constexpr float kHalfMax = 65504.f;
            for (size_t i = 0; i < total_floats; ++i) {
                float v = decoded[i];
                if (!(v == v)) v = 0.f;           // NaN
                else if (v > kHalfMax) v = kHalfMax;
                else if (v < 0.f) v = 0.f;
                decoded[i] = v;
            }

            vector<uint8_t> pixels;
            pixels.resize(total_floats * sizeof(uint16_t));
            floats_to_halves(decoded, reinterpret_cast<uint16_t*>(pixels.data()), total_floats);
            env->init(inner->uri(), w, h, std::move(pixels));
        }
        stbi_image_free(decoded);
    }

    return env;
}

} // namespace velk::ui::impl
