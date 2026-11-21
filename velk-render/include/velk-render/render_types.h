#ifndef VELK_RENDER_TYPES_H
#define VELK_RENDER_TYPES_H

#include <velk/api/math_types.h>

#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_program.h>

#include <cstdint>
#include <cstring>

namespace velk {

/// Face culling mode, shared between material options and pipeline creation.
enum class CullMode : uint8_t
{
    None,   ///< No culling. Default; matches glTF doubleSided = true.
    Back,   ///< Cull back faces. glTF single-sided default.
    Front,  ///< Cull front faces. Useful for skyboxes and shadow tricks.
};

/// Blend state for the color attachment of a pipeline.
enum class BlendMode : uint8_t
{
    Alpha,   ///< Standard alpha blending (src.rgb * src.a + dst.rgb * (1 - src.a)). Default.
    Opaque,  ///< No blending; fragment overwrites destination.
};

/// Depth compare op, by distance from the camera: `Less` passes what is nearer than what
/// is already drawn. `Disabled` turns depth testing off entirely.
enum class CompareOp : uint8_t
{
    Disabled,
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

/// Depth attachment format for a render target.
enum class DepthFormat : uint8_t
{
    None,     ///< No depth attachment.
    Default,  ///< Backend picks a sensible default (currently D32_SFLOAT).
};

/// Pixel format for textures.
enum class PixelFormat : uint8_t
{
    RGBA8,      ///< 4 bytes per pixel, linear color.
    RGBA8_SRGB, ///< 4 bytes per pixel, sRGB-tagged (auto-linearised on sample).
    R8,         ///< 1 byte per pixel, glyph atlases.
    RGBA16F,    ///< 8 bytes per pixel, 16-bit float per channel (HDR).
    RGBA32F,    ///< 16 bytes per pixel, 32-bit float per channel (precision-critical, e.g. world position).
    RG32UI,     ///< 8 bytes per pixel, two 32-bit unsigned integers (IDs). Read with texelFetch.
    R32F,       ///< 4 bytes per pixel, one 32-bit float. Read with texelFetch.

    // Block compressed, for sampling only. Their textures have the mip levels their data holds.
    BC1_RGBA,        ///< 8 bytes a 4 by 4 block, 1 bit alpha.
    BC1_RGBA_SRGB,   ///< BC1, sRGB.
    BC2_RGBA,        ///< 16 bytes a 4 by 4 block, 4 bit alpha.
    BC2_RGBA_SRGB,   ///< BC2, sRGB.
    BC3_RGBA,        ///< 16 bytes a 4 by 4 block, interpolated alpha.
    BC3_RGBA_SRGB,   ///< BC3, sRGB.
    BC4_R,           ///< 8 bytes a 4 by 4 block, one channel.
    BC5_RG,          ///< 16 bytes a 4 by 4 block, two channels (normal maps).
    BC6H_RGB_UFLOAT, ///< 16 bytes a 4 by 4 block, unsigned half float colour (HDR).
    BC6H_RGB_SFLOAT, ///< 16 bytes a 4 by 4 block, signed half float colour (HDR).
    BC7_RGBA,        ///< 16 bytes a 4 by 4 block.
    BC7_RGBA_SRGB,   ///< BC7, sRGB.
    ETC2_RGB8,       ///< 8 bytes a 4 by 4 block.
    ETC2_RGB8_SRGB,  ///< ETC2 RGB, sRGB.
    ETC2_RGBA8,      ///< 16 bytes a 4 by 4 block, with EAC alpha.
    ETC2_RGBA8_SRGB, ///< ETC2 RGBA, sRGB.
    EAC_R11,         ///< 8 bytes a 4 by 4 block, one channel.
    EAC_RG11,        ///< 16 bytes a 4 by 4 block, two channels.
    ASTC_4x4,        ///< 16 bytes a 4 by 4 block.
    ASTC_4x4_SRGB,   ///< ASTC 4 by 4, sRGB.
    ASTC_5x5,        ///< 16 bytes a 5 by 5 block.
    ASTC_5x5_SRGB,   ///< ASTC 5 by 5, sRGB.
    ASTC_6x6,        ///< 16 bytes a 6 by 6 block.
    ASTC_6x6_SRGB,   ///< ASTC 6 by 6, sRGB.
    ASTC_8x8,        ///< 16 bytes an 8 by 8 block.
    ASTC_8x8_SRGB,   ///< ASTC 8 by 8, sRGB.
};

/// How a pixel format lays out its texels: in blocks of block_width by block_height texels of
/// block_bytes each, 1 by 1 for an uncompressed format.
struct PixelFormatLayout
{
    uint32_t block_width = 1;
    uint32_t block_height = 1;
    uint32_t block_bytes = 4;
};

/// The layout of @p format.
constexpr PixelFormatLayout pixel_format_layout(PixelFormat format)
{
    switch (format) {
        case PixelFormat::RGBA8:
        case PixelFormat::RGBA8_SRGB:
        case PixelFormat::R32F:            return {1, 1, 4};
        case PixelFormat::R8:              return {1, 1, 1};
        case PixelFormat::RGBA16F:
        case PixelFormat::RG32UI:          return {1, 1, 8};
        case PixelFormat::RGBA32F:         return {1, 1, 16};
        case PixelFormat::BC1_RGBA:
        case PixelFormat::BC1_RGBA_SRGB:
        case PixelFormat::BC4_R:
        case PixelFormat::ETC2_RGB8:
        case PixelFormat::ETC2_RGB8_SRGB:
        case PixelFormat::EAC_R11:         return {4, 4, 8};
        case PixelFormat::ASTC_5x5:
        case PixelFormat::ASTC_5x5_SRGB:   return {5, 5, 16};
        case PixelFormat::ASTC_6x6:
        case PixelFormat::ASTC_6x6_SRGB:   return {6, 6, 16};
        case PixelFormat::ASTC_8x8:
        case PixelFormat::ASTC_8x8_SRGB:   return {8, 8, 16};
        default:                           return {4, 4, 16};
    }
}

/// Whether @p format is block compressed.
constexpr bool is_block_compressed(PixelFormat format)
{
    return static_cast<uint8_t>(format) >= static_cast<uint8_t>(PixelFormat::BC1_RGBA);
}

/// Whether @p format is sRGB-tagged.
constexpr bool is_srgb(PixelFormat format)
{
    switch (format) {
        case PixelFormat::RGBA8_SRGB:
        case PixelFormat::BC1_RGBA_SRGB:
        case PixelFormat::BC2_RGBA_SRGB:
        case PixelFormat::BC3_RGBA_SRGB:
        case PixelFormat::BC7_RGBA_SRGB:
        case PixelFormat::ETC2_RGB8_SRGB:
        case PixelFormat::ETC2_RGBA8_SRGB:
        case PixelFormat::ASTC_4x4_SRGB:
        case PixelFormat::ASTC_5x5_SRGB:
        case PixelFormat::ASTC_6x6_SRGB:
        case PixelFormat::ASTC_8x8_SRGB:   return true;
        default:                           return false;
    }
}

/// @p format with its colour space sRGB when @p srgb, linear otherwise; @p format itself for a
/// format without both.
constexpr PixelFormat with_srgb(PixelFormat format, bool srgb)
{
    switch (format) {
        case PixelFormat::RGBA8:
        case PixelFormat::RGBA8_SRGB:      return srgb ? PixelFormat::RGBA8_SRGB : PixelFormat::RGBA8;
        case PixelFormat::BC1_RGBA:
        case PixelFormat::BC1_RGBA_SRGB:   return srgb ? PixelFormat::BC1_RGBA_SRGB : PixelFormat::BC1_RGBA;
        case PixelFormat::BC2_RGBA:
        case PixelFormat::BC2_RGBA_SRGB:   return srgb ? PixelFormat::BC2_RGBA_SRGB : PixelFormat::BC2_RGBA;
        case PixelFormat::BC3_RGBA:
        case PixelFormat::BC3_RGBA_SRGB:   return srgb ? PixelFormat::BC3_RGBA_SRGB : PixelFormat::BC3_RGBA;
        case PixelFormat::BC7_RGBA:
        case PixelFormat::BC7_RGBA_SRGB:   return srgb ? PixelFormat::BC7_RGBA_SRGB : PixelFormat::BC7_RGBA;
        case PixelFormat::ETC2_RGB8:
        case PixelFormat::ETC2_RGB8_SRGB:  return srgb ? PixelFormat::ETC2_RGB8_SRGB : PixelFormat::ETC2_RGB8;
        case PixelFormat::ETC2_RGBA8:
        case PixelFormat::ETC2_RGBA8_SRGB: return srgb ? PixelFormat::ETC2_RGBA8_SRGB : PixelFormat::ETC2_RGBA8;
        case PixelFormat::ASTC_4x4:
        case PixelFormat::ASTC_4x4_SRGB:   return srgb ? PixelFormat::ASTC_4x4_SRGB : PixelFormat::ASTC_4x4;
        case PixelFormat::ASTC_5x5:
        case PixelFormat::ASTC_5x5_SRGB:   return srgb ? PixelFormat::ASTC_5x5_SRGB : PixelFormat::ASTC_5x5;
        case PixelFormat::ASTC_6x6:
        case PixelFormat::ASTC_6x6_SRGB:   return srgb ? PixelFormat::ASTC_6x6_SRGB : PixelFormat::ASTC_6x6;
        case PixelFormat::ASTC_8x8:
        case PixelFormat::ASTC_8x8_SRGB:   return srgb ? PixelFormat::ASTC_8x8_SRGB : PixelFormat::ASTC_8x8;
        default:                           return format;
    }
}

/// Bytes of a @p width by @p height level of @p format, whole blocks.
constexpr size_t pixel_format_level_bytes(PixelFormat format, uint32_t width, uint32_t height)
{
    const PixelFormatLayout l = pixel_format_layout(format);
    return size_t((width + l.block_width - 1) / l.block_width) * size_t((height + l.block_height - 1) / l.block_height)
           * l.block_bytes;
}

/// Format of a surface's per-frame composite intermediate. Whitelisted
/// subset of `PixelFormat`; numeric values match `PixelFormat` so a
/// `static_cast<PixelFormat>(scf)` is safe and round-trips.
enum class SurfaceColorFormat : uint8_t
{
    /// 4 bytes per pixel, sRGB-tagged. Linear writes from raster /
    /// blits get sRGB-encoded automatically. AFBC/UBWC-friendly on
    /// tile-based mobile GPUs. Bright (>1.0) values clamp on store —
    /// fine for tonemapped or SDR-native content. Default.
    RGBA8_SRGB = static_cast<uint8_t>(PixelFormat::RGBA8_SRGB),
    /// 8 bytes per pixel, linear half-float. Preserves values >1.0 so
    /// HDR producers (bloom-style, non-tonemapped HDR display) keep
    /// their full range. Disables AFBC/UBWC; opt in only when needed.
    RGBA16F    = static_cast<uint8_t>(PixelFormat::RGBA16F),
};

/// Front-face winding convention. Pairs with CullMode.
enum class FrontFace : uint8_t
{
    CounterClockwise,  ///< CCW triangles are front (right-handed / glTF default).
    Clockwise,         ///< CW triangles are front.
};

/// Selector for context-owned default buffers. Each entry names an
/// optional vertex-stream slot; the context owns a single shared
/// fallback per slot that draws point at when the primitive does not
/// supply its own. New slots (e.g. default tangents, TEXCOORD_2) can
/// be added here without growing IRenderContext's API.
enum class DefaultBufferType : uint8_t
{
    Uv1,  ///< One vec2 (0, 0); for draws without kDrawFlagUv1, whose vertices all read it.
    Color,  ///< One white RGBA8 word; for draws without kDrawFlagColor likewise.
};

/// Texture addressing mode for coordinates outside [0, 1]. Matches
/// glTF 2.0's sampler wrap modes and the GPU's stock address options.
enum class SamplerAddressMode : uint8_t
{
    Repeat,          ///< uv mod 1. Default; matches glTF spec default.
    MirroredRepeat,  ///< Ping-pong on each integer boundary.
    ClampToEdge,     ///< Clamp to [0, 1]; out-of-range samples return the edge texel.
};

/// Texture magnification / minification filter.
enum class SamplerFilter : uint8_t
{
    Nearest,  ///< Nearest-neighbour sampling.
    Linear,   ///< Bilinear filtering. Default.
};

/// Texture mipmap mode. Chosen alongside the min filter.
enum class SamplerMipmapMode : uint8_t
{
    Nearest,  ///< Pick the nearest mip level.
    Linear,   ///< Trilinear (interpolate between two mip levels). Default.
};

/// Sampler configuration for a texture. Honoured per-texture by the
/// backend: each distinct `SamplerDesc` the renderer sees materialises
/// one underlying GPU sampler (cached). Matches the slot structure of
/// glTF 2.0 samplers.
struct SamplerDesc
{
    SamplerAddressMode wrap_s{SamplerAddressMode::Repeat};
    SamplerAddressMode wrap_t{SamplerAddressMode::Repeat};
    SamplerAddressMode wrap_r{SamplerAddressMode::Repeat};
    SamplerFilter mag_filter{SamplerFilter::Linear};
    SamplerFilter min_filter{SamplerFilter::Linear};
    SamplerMipmapMode mipmap_mode{SamplerMipmapMode::Linear};

    constexpr bool operator==(const SamplerDesc& rhs) const
    {
        return wrap_s == rhs.wrap_s && wrap_t == rhs.wrap_t && wrap_r == rhs.wrap_r
            && mag_filter == rhs.mag_filter && min_filter == rhs.min_filter
            && mipmap_mode == rhs.mipmap_mode;
    }
    constexpr bool operator!=(const SamplerDesc& rhs) const { return !(*this == rhs); }
};

/// Pipeline keys used by the render context's auto-assign counter.
/// Visuals that own a built-in pipeline provide their own stable key
/// (typically via make_hash64 on the class name).
namespace PipelineKey {
inline constexpr uint64_t Default = 1;       ///< Filled rect
inline constexpr uint64_t CustomBase = 1000; ///< Auto-assigned keys start here.
} // namespace PipelineKey

/// Maximum inline instance data size in a DrawEntry. Sized for the
/// universal `ElementInstance` (128 bytes: mat4 world_matrix + vec4
/// offset + vec4 size + vec4 color + uvec4 params).
inline constexpr uint32_t kMaxInstanceDataSize = 128;

/**
 * @brief Generic draw entry produced by visuals.
 *
 * Visuals pack their own instance data matching their pipeline's vertex input.
 * The renderer groups entries by (pipeline_key, primitive, texture_key),
 * concatenates instance data into batches, and applies the world transform.
 *
 * Convention: the first two floats in instance_data are element-local (x, y).
 * The renderer offsets them by the element's world position.
 *
 * `primitive` is optional. When null, the renderer substitutes the unit
 * quad's single primitive so 2D visuals that don't care about geometry
 * keep working.
 */
struct DrawEntry
{
    uint64_t pipeline_key{};                       ///< Visual's own pipeline (used when no material overrides).
    uint64_t texture_key{};                        ///< Texture binding (0 = none).
    rect bounds{};                                 ///< Element-local bounds.
    IMeshPrimitive::Ptr primitive{};               ///< Geometry primitive; null = unit quad.
    IProgram::Ptr material{};                      ///< Material driving this entry's pipeline + data (0 = visual's own shader).
    uint64_t pipeline_override{};                  ///< Cached material pipeline handle; filled by the renderer.
    uint8_t instance_data[kMaxInstanceDataSize]{}; ///< Packed instance data for the GPU.
    uint32_t instance_size{};                      ///< Bytes used in instance_data, or the size of one instance in instance_source.
    IInterface::Ptr instance_source{};             ///< When set, supplies many instances in place of instance_data.
    uint32_t instance_source_mesh{};               ///< Which of instance_source's meshes this entry draws.

    /**
     * @brief Packs a typed instance struct into instance_data.
     * @tparam T Instance struct type (typically ElementInstance).
     */
    template <typename T>
    void set_instance(const T& inst)
    {
        static_assert(sizeof(T) <= kMaxInstanceDataSize, "Instance data exceeds DrawEntry capacity");
        std::memcpy(instance_data, &inst, sizeof(T));
        instance_size = sizeof(T);
    }

    /**
     * @brief Accesses instance_data as a typed struct.
     * @tparam T Instance struct type to interpret the data as.
     */
    template <typename T>
    T& as_instance() { return *reinterpret_cast<T*>(instance_data); }

    /** @brief Const overload of as_instance(). */
    template <typename T>
    const T& as_instance() const { return *reinterpret_cast<const T*>(instance_data); }
};

/** @brief Selects the GPU backend used by the render context. */
enum class RenderBackendType : uint8_t
{
    Default, ///< Platform best: Vulkan on Windows/Linux/Android, Metal on Apple.
    Vulkan,  ///< Explicit Vulkan backend.
};

/**
 * @brief Configuration for creating a render context.
 *
 * Passed to velk::create_render_context() / IRenderContext::init().
 */
struct RenderConfig
{
    RenderBackendType backend = RenderBackendType::Default; ///< GPU backend selection.
    void* backend_params = nullptr;                         ///< Backend-specific init params (e.g. VulkanInitParams*).
};

/**
 * @brief Controls how often a surface is presented.
 *
 * Affects the swapchain present mode chosen by the backend, plus optional
 * software pacing in the runtime layer.
 */
enum class UpdateRate : uint8_t
{
    VSync,     ///< Cap to display refresh (FIFO present mode). Default. No tearing.
    Unlimited, ///< Render as fast as possible (IMMEDIATE/MAILBOX). May tear.
    Targeted   ///< Software-paced to @c target_fps via std::this_thread::sleep_until.
};

/**
 * @brief User-facing configuration for creating a render surface.
 *
 * Passed to IRenderContext::create_surface() to describe the desired surface.
 * Mirrored later into SurfaceDesc for the backend.
 */
struct SurfaceConfig
{
    int width{};                                ///< Surface width in pixels.
    int height{};                               ///< Surface height in pixels.
    UpdateRate update_rate{UpdateRate::VSync};  ///< Pacing mode (VSync, Unlimited, Targeted).
    int target_fps{60};                         ///< Target framerate for UpdateRate::Targeted (ignored otherwise).
    DepthFormat depth{DepthFormat::None};       ///< Depth attachment. None for flat UI, Default for 3D scenes.
    SurfaceColorFormat color_format{SurfaceColorFormat::RGBA8_SRGB}; ///< Composite format. RGBA16F for HDR / non-tonemapped bloom.
};

} // namespace velk

#endif // VELK_RENDER_TYPES_H
