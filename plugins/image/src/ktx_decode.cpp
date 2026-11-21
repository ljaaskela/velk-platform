#include "ktx_decode.h"

#include <velk/api/velk.h>

#include <ktx.h>
// libktx's own lists of formats: VkFormat values, and the OpenGL formats of KTX1 files in them.
#include "vkformat_enum.h"
#include "vk_format.h"

#include <cstdlib>
#include <cstring>

// libktx's software ETC unpacker (lib/etcunpack.cxx), which its OpenGL upload uses: unpacks one
// ETC1, ETC2 or EAC image to uncompressed texels in memory it allocates with malloc.
extern "C" KTX_error_code _ktxUnpackETC(const unsigned char* srcETC, const unsigned int srcFormat,
                                        ktx_uint32_t active_width, ktx_uint32_t active_height,
                                        unsigned char** dstImage, unsigned int* format,
                                        unsigned int* internalFormat, unsigned int* type,
                                        int R16Formats, unsigned char supportsSRGB);

namespace velk::ui::impl {

namespace {

// OpenGL's names for what _ktxUnpackETC reads and writes.
constexpr unsigned int kGlRed = 0x1903;
constexpr unsigned int kGlRg = 0x8227;
constexpr unsigned int kGlRgb = 0x1907;
constexpr unsigned int kGlRgba = 0x1908;
constexpr unsigned int kGlEtc2Rgb8 = 0x9274;
constexpr unsigned int kGlEtc2Srgb8 = 0x9275;
constexpr unsigned int kGlEtc2Rgba8 = 0x9278;
constexpr unsigned int kGlEtc2Srgb8Alpha8 = 0x9279;
constexpr unsigned int kGlEacR11 = 0x9270;
constexpr unsigned int kGlEacRg11 = 0x9272;

/// The format a VkFormat uploads as, or false where there is none.
bool native_format(uint32_t vk, PixelFormat& out)
{
    switch (vk) {
        case VK_FORMAT_R8G8B8A8_UNORM:            out = PixelFormat::RGBA8; return true;
        case VK_FORMAT_R8G8B8A8_SRGB:             out = PixelFormat::RGBA8_SRGB; return true;
        case VK_FORMAT_R8_UNORM:                  out = PixelFormat::R8; return true;
        case VK_FORMAT_R16G16B16A16_SFLOAT:       out = PixelFormat::RGBA16F; return true;
        case VK_FORMAT_R32G32B32A32_SFLOAT:       out = PixelFormat::RGBA32F; return true;
        case VK_FORMAT_R32_SFLOAT:                out = PixelFormat::R32F; return true;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:      out = PixelFormat::BC1_RGBA; return true;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:       out = PixelFormat::BC1_RGBA_SRGB; return true;
        case VK_FORMAT_BC2_UNORM_BLOCK:           out = PixelFormat::BC2_RGBA; return true;
        case VK_FORMAT_BC2_SRGB_BLOCK:            out = PixelFormat::BC2_RGBA_SRGB; return true;
        case VK_FORMAT_BC3_UNORM_BLOCK:           out = PixelFormat::BC3_RGBA; return true;
        case VK_FORMAT_BC3_SRGB_BLOCK:            out = PixelFormat::BC3_RGBA_SRGB; return true;
        case VK_FORMAT_BC4_UNORM_BLOCK:           out = PixelFormat::BC4_R; return true;
        case VK_FORMAT_BC5_UNORM_BLOCK:           out = PixelFormat::BC5_RG; return true;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:         out = PixelFormat::BC6H_RGB_UFLOAT; return true;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:         out = PixelFormat::BC6H_RGB_SFLOAT; return true;
        case VK_FORMAT_BC7_UNORM_BLOCK:           out = PixelFormat::BC7_RGBA; return true;
        case VK_FORMAT_BC7_SRGB_BLOCK:            out = PixelFormat::BC7_RGBA_SRGB; return true;
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:   out = PixelFormat::ETC2_RGB8; return true;
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:    out = PixelFormat::ETC2_RGB8_SRGB; return true;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: out = PixelFormat::ETC2_RGBA8; return true;
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:  out = PixelFormat::ETC2_RGBA8_SRGB; return true;
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:       out = PixelFormat::EAC_R11; return true;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:    out = PixelFormat::EAC_RG11; return true;
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:      out = PixelFormat::ASTC_4x4; return true;
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:       out = PixelFormat::ASTC_4x4_SRGB; return true;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:      out = PixelFormat::ASTC_5x5; return true;
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:       out = PixelFormat::ASTC_5x5_SRGB; return true;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:      out = PixelFormat::ASTC_6x6; return true;
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:       out = PixelFormat::ASTC_6x6_SRGB; return true;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:      out = PixelFormat::ASTC_8x8; return true;
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:       out = PixelFormat::ASTC_8x8_SRGB; return true;
        default:                                  return false;
    }
}

/// An uncompressed layout widened on load: its channels, bytes a channel, whether it is BGR,
/// and what it becomes.
struct Widened
{
    uint32_t channels = 0;
    uint32_t channel_bytes = 0;
    bool bgr = false;
    PixelFormat to = PixelFormat::RGBA8;
};

bool widened_format(uint32_t vk, Widened& out)
{
    switch (vk) {
        case VK_FORMAT_R8G8_UNORM:          out = {2, 1, false, PixelFormat::RGBA8}; return true;
        case VK_FORMAT_R8G8B8_UNORM:        out = {3, 1, false, PixelFormat::RGBA8}; return true;
        case VK_FORMAT_R8G8B8_SRGB:         out = {3, 1, false, PixelFormat::RGBA8_SRGB}; return true;
        case VK_FORMAT_B8G8R8_UNORM:        out = {3, 1, true, PixelFormat::RGBA8}; return true;
        case VK_FORMAT_B8G8R8_SRGB:         out = {3, 1, true, PixelFormat::RGBA8_SRGB}; return true;
        case VK_FORMAT_B8G8R8A8_UNORM:      out = {4, 1, true, PixelFormat::RGBA8}; return true;
        case VK_FORMAT_B8G8R8A8_SRGB:       out = {4, 1, true, PixelFormat::RGBA8_SRGB}; return true;
        case VK_FORMAT_R16_SFLOAT:          out = {1, 2, false, PixelFormat::RGBA16F}; return true;
        case VK_FORMAT_R16G16_SFLOAT:       out = {2, 2, false, PixelFormat::RGBA16F}; return true;
        case VK_FORMAT_R16G16B16_SFLOAT:    out = {3, 2, false, PixelFormat::RGBA16F}; return true;
        case VK_FORMAT_R32G32_SFLOAT:       out = {2, 4, false, PixelFormat::RGBA32F}; return true;
        case VK_FORMAT_R32G32B32_SFLOAT:    out = {3, 4, false, PixelFormat::RGBA32F}; return true;
        default:                            return false;
    }
}

/// The OpenGL format _ktxUnpackETC knows @p format by, 0 for one it does not.
unsigned int etc_gl_format(PixelFormat format)
{
    switch (format) {
        case PixelFormat::ETC2_RGB8:       return kGlEtc2Rgb8;
        case PixelFormat::ETC2_RGB8_SRGB:  return kGlEtc2Srgb8;
        case PixelFormat::ETC2_RGBA8:      return kGlEtc2Rgba8;
        case PixelFormat::ETC2_RGBA8_SRGB: return kGlEtc2Srgb8Alpha8;
        case PixelFormat::EAC_R11:         return kGlEacR11;
        case PixelFormat::EAC_RG11:        return kGlEacRg11;
        default:                           return 0;
    }
}

bool is_astc_vk(uint32_t vk)
{
    return vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}

bool sampled_has(const vector<PixelFormat>& sampled, PixelFormat f)
{
    for (PixelFormat s : sampled) {
        if (s == f) return true;
    }
    return false;
}

/// Whether the device samples @p f: an uncompressed format always.
bool can_sample(const vector<PixelFormat>& sampled, PixelFormat f)
{
    return !is_block_compressed(f) || sampled_has(sampled, f);
}

uint32_t level_size(uint32_t base, uint32_t level)
{
    const uint32_t s = base >> level;
    return s > 0 ? s : 1u;
}

/// A half float's bits for 1.
constexpr uint16_t kHalfOne = 0x3C00;

/// Widens one level of @p w by @p h texels, rows @p pitch bytes apart, into four channels.
void widen_level(const uint8_t* src, size_t pitch, uint32_t w, uint32_t h, const Widened& f, vector<uint8_t>& out)
{
    const size_t dst_texel = 4u * f.channel_bytes;
    const size_t start = out.size();
    out.resize(start + size_t(w) * h * dst_texel);
    uint8_t* dst = out.data() + start;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = src + y * pitch;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = row + size_t(x) * f.channels * f.channel_bytes;
            uint8_t* d = dst + (size_t(y) * w + x) * dst_texel;
            for (uint32_t c = 0; c < 4; ++c) {
                uint32_t from = c;
                if (f.bgr && c < 3) from = 2 - c;
                uint8_t* dc = d + c * f.channel_bytes;
                if (from < f.channels) {
                    std::memcpy(dc, s + from * f.channel_bytes, f.channel_bytes);
                } else if (c == 3) {
                    // Opaque where the layout has no alpha.
                    if (f.channel_bytes == 1) {
                        dc[0] = 255;
                    } else if (f.channel_bytes == 2) {
                        std::memcpy(dc, &kHalfOne, 2);
                    } else {
                        const float one = 1.f;
                        std::memcpy(dc, &one, 4);
                    }
                } else {
                    std::memset(dc, 0, f.channel_bytes);
                }
            }
        }
    }
}

/// Unpacks one ETC level of @p w by @p h texels at @p src to RGBA8, appended to @p out.
bool unpack_etc_level(const uint8_t* src, unsigned int gl_format, uint32_t w, uint32_t h, vector<uint8_t>& out)
{
    unsigned char* unpacked = nullptr;
    unsigned int format = 0;
    unsigned int internal_format = 0;
    unsigned int type = 0;
    if (_ktxUnpackETC(src, gl_format, w, h, &unpacked, &format, &internal_format, &type, 0, 1) != KTX_SUCCESS
        || !unpacked) {
        return false;
    }
    const uint32_t channels = format == kGlRed ? 1u : format == kGlRg ? 2u : format == kGlRgb ? 3u : 4u;
    // Its rows are packed to 4 bytes, as OpenGL unpacks them by default.
    const size_t pitch = (size_t(w) * channels + 3u) & ~size_t(3);
    widen_level(unpacked, pitch, w, h, Widened{channels, 1, false, PixelFormat::RGBA8}, out);
    std::free(unpacked);
    return true;
}

/// The averages of the Basis file in @p bytes, from its first level of at most 64 by 64 texels
/// (or its smallest), which it is transcoded to RGBA8 for; unknown where that fails.
ImageAverages basis_averages(const uint8_t* bytes, size_t size)
{
    ImageAverages out;
    ktxTexture2* tex = nullptr;
    if (ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex) != KTX_SUCCESS) {
        return out;
    }
    if (ktxTexture2_TranscodeBasis(tex, KTX_TTF_RGBA32, 0) == KTX_SUCCESS) {
        ktxTexture* t = ktxTexture(tex);
        // A small level is mean enough of the whole and quick to add up.
        uint32_t level = 0;
        while (level + 1 < t->numLevels && level_size(t->baseWidth, level) * level_size(t->baseHeight, level) > 64u * 64u) {
            ++level;
        }
        ktx_size_t offset = 0;
        if (ktxTexture_GetImageOffset(t, level, 0, 0, &offset) == KTX_SUCCESS) {
            out = average_rgba8(ktxTexture_GetData(t) + offset,
                                size_t(level_size(t->baseWidth, level)) * level_size(t->baseHeight, level));
        }
    }
    ktxTexture_Destroy(ktxTexture(tex));
    return out;
}

} // namespace

bool is_ktx(const uint8_t* bytes, size_t size)
{
    static constexpr uint8_t kKtx1[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
    static constexpr uint8_t kKtx2[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    return size >= 12 && (std::memcmp(bytes, kKtx1, 12) == 0 || std::memcmp(bytes, kKtx2, 12) == 0);
}

KtxImage decode_ktx(const uint8_t* bytes, size_t size, const vector<PixelFormat>& sampled, string_view uri)
{
    KtxImage out;
    ktxTexture* tex = nullptr;
    KTX_error_code rc = ktxTexture_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
    if (rc != KTX_SUCCESS || !tex) {
        VELK_LOG(E, "KTX: '%.*s' could not be read: %s", int(uri.size()), uri.data(), ktxErrorString(rc));
        return out;
    }

    uint32_t vk = VK_FORMAT_UNDEFINED;
    bool basis = false;
    if (tex->classId == ktxTexture2_c) {
        ktxTexture2* tex2 = reinterpret_cast<ktxTexture2*>(tex);
        if (ktxTexture2_NeedsTranscoding(tex2)) {
            // To the best block format the device samples, else uncompressed.
            basis = true;
            ktx_transcode_fmt_e target = KTX_TTF_RGBA32;
            if (can_sample(sampled, PixelFormat::BC7_RGBA) || can_sample(sampled, PixelFormat::BC7_RGBA_SRGB)) {
                target = KTX_TTF_BC7_RGBA;
            } else if (can_sample(sampled, PixelFormat::ASTC_4x4) || can_sample(sampled, PixelFormat::ASTC_4x4_SRGB)) {
                target = KTX_TTF_ASTC_4x4_RGBA;
            } else if (can_sample(sampled, PixelFormat::ETC2_RGBA8) || can_sample(sampled, PixelFormat::ETC2_RGBA8_SRGB)) {
                target = KTX_TTF_ETC2_RGBA;
            }
            rc = ktxTexture2_TranscodeBasis(tex2, target, 0);
            if (rc != KTX_SUCCESS) {
                VELK_LOG(E, "KTX: '%.*s' could not be transcoded: %s", int(uri.size()), uri.data(), ktxErrorString(rc));
                ktxTexture_Destroy(tex);
                return out;
            }
        }
        vk = tex2->vkFormat;
        // ASTC the device does not sample, or of a block size without a format here: decoded.
        PixelFormat native{};
        if (is_astc_vk(vk) && (!native_format(vk, native) || !can_sample(sampled, native))) {
            rc = ktxTexture2_DecodeAstc(tex2);
            if (rc != KTX_SUCCESS) {
                VELK_LOG(E, "KTX: '%.*s' ASTC could not be decoded: %s", int(uri.size()), uri.data(), ktxErrorString(rc));
                ktxTexture_Destroy(tex);
                return out;
            }
            vk = tex2->vkFormat;
        }
    } else {
        vk = vkGetFormatFromOpenGLInternalFormat(reinterpret_cast<ktxTexture1*>(tex)->glInternalformat);
    }

    if (tex->numLayers > 1 || tex->numFaces > 1 || tex->baseDepth > 1) {
        VELK_LOG(W, "KTX: '%.*s' is an array, a cube map or 3D; its first 2D image is used", int(uri.size()),
                 uri.data());
    }

    const uint32_t width = tex->baseWidth;
    const uint32_t height = tex->baseHeight;
    const uint32_t levels = tex->numLevels > 0 ? tex->numLevels : 1u;
    const uint8_t* data = ktxTexture_GetData(tex);

    PixelFormat format{};
    Widened widen{};
    bool ok = true;
    if (native_format(vk, format) && can_sample(sampled, format)) {
        // As it is: each level's first image, rows as libktx keeps them.
        for (uint32_t l = 0; l < levels && ok; ++l) {
            ktx_size_t offset = 0;
            ok = ktxTexture_GetImageOffset(tex, l, 0, 0, &offset) == KTX_SUCCESS;
            const uint32_t w = level_size(width, l);
            const uint32_t h = level_size(height, l);
            if (!ok) break;
            if (is_block_compressed(format)) {
                const size_t bytes_level = pixel_format_level_bytes(format, w, h);
                const size_t at = out.data.size();
                out.data.resize(at + bytes_level);
                std::memcpy(out.data.data() + at, data + offset, bytes_level);
            } else {
                const size_t row = size_t(w) * pixel_format_layout(format).block_bytes;
                const size_t pitch = ktxTexture_GetRowPitch(tex, l);
                const size_t at = out.data.size();
                out.data.resize(at + row * h);
                for (uint32_t y = 0; y < h; ++y) {
                    std::memcpy(out.data.data() + at + y * row, data + offset + y * pitch, row);
                }
            }
        }
    } else if (native_format(vk, format) && etc_gl_format(format) != 0) {
        // ETC the device does not sample: unpacked to RGBA8.
        const unsigned int gl = etc_gl_format(format);
        const bool srgb = is_srgb(format);
        for (uint32_t l = 0; l < levels && ok; ++l) {
            ktx_size_t offset = 0;
            ok = ktxTexture_GetImageOffset(tex, l, 0, 0, &offset) == KTX_SUCCESS
                 && unpack_etc_level(data + offset, gl, level_size(width, l), level_size(height, l), out.data);
        }
        format = srgb ? PixelFormat::RGBA8_SRGB : PixelFormat::RGBA8;
    } else if (widened_format(vk, widen)) {
        for (uint32_t l = 0; l < levels && ok; ++l) {
            ktx_size_t offset = 0;
            ok = ktxTexture_GetImageOffset(tex, l, 0, 0, &offset) == KTX_SUCCESS;
            if (ok) {
                widen_level(data + offset, ktxTexture_GetRowPitch(tex, l), level_size(width, l), level_size(height, l),
                            widen, out.data);
            }
        }
        format = widen.to;
    } else {
        VELK_LOG(E, "KTX: '%.*s' has a format this device cannot use (VkFormat %u)", int(uri.size()), uri.data(), vk);
        ok = false;
    }

    if (ok) {
        out.width = int(width);
        out.height = int(height);
        out.format = format;
        out.levels = levels;
        if (format == PixelFormat::RGBA8 || format == PixelFormat::RGBA8_SRGB) {
            out.averages = average_rgba8(out.data.data(), size_t(width) * height);
        }
    } else {
        out.data = vector<uint8_t>{};
    }
    ktxTexture_Destroy(tex);

    // A transcoded file's texels are not readable as they are: worked out from its RGBA8.
    if (ok && basis && !out.averages.known) {
        out.averages = basis_averages(bytes, size);
    }
    return out;
}

} // namespace velk::ui::impl
