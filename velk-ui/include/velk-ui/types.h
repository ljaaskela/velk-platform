#ifndef VELK_UI_TYPES_H
#define VELK_UI_TYPES_H

#include <velk/api/math_types.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk_ui {

/// Well-known pipeline keys used by built-in visuals and consumed by backends.
namespace PipelineKey {
inline constexpr uint64_t Rect = 1;
inline constexpr uint64_t Text = 2;
inline constexpr uint64_t RoundedRect = 3;
inline constexpr uint64_t Gradient = 4;
inline constexpr uint64_t CustomBase = 1000;
} // namespace PipelineKey

/// Well-known texture keys.
namespace TextureKey {
inline constexpr uint64_t Atlas = 1;
} // namespace TextureKey

/// Maximum inline instance data size in a DrawEntry.
inline constexpr uint32_t kMaxInstanceDataSize = 64;

/**
 * @brief Generic draw entry produced by IVisual.
 *
 * Visuals pack their own instance data matching their pipeline's vertex input.
 * The renderer groups entries by (pipeline_key, texture_key), concatenates
 * instance data into batches, and applies the world transform.
 *
 * Convention: the first two floats in instance_data are element-local (x, y).
 * The renderer offsets them by the element's world position.
 */
struct DrawEntry
{
    uint64_t pipeline_key{};        ///< Pipeline to draw with.
    uint64_t texture_key{};         ///< Texture binding (0 = none).
    velk::rect bounds{};            ///< Element-local bounds (used for per-element uniforms).
    uint8_t instance_data[kMaxInstanceDataSize]{}; ///< Packed instance data for the GPU.
    uint32_t instance_size{};       ///< Bytes used in instance_data.
};

enum class DirtyFlags : uint8_t
{
    None = 0,
    Layout = 1 << 0,
    Visual = 1 << 1,
    DrawOrder = 1 << 2,
    All = 0xff,
};

inline constexpr DirtyFlags operator|(DirtyFlags a, DirtyFlags b)
{
    return static_cast<DirtyFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr DirtyFlags operator&(DirtyFlags a, DirtyFlags b)
{
    return static_cast<DirtyFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline constexpr DirtyFlags& operator|=(DirtyFlags& a, DirtyFlags b)
{
    a = a | b;
    return a;
}

inline constexpr DirtyFlags& operator&=(DirtyFlags& a, DirtyFlags b)
{
    a = a & b;
    return a;
}

inline constexpr DirtyFlags operator~(DirtyFlags a)
{
    return static_cast<DirtyFlags>(~static_cast<uint8_t>(a));
}

enum class DimUnit : uint8_t
{
    None,
    Px,
    Pct
};

struct dim
{
    float value{};
    DimUnit unit{DimUnit::None};

    static constexpr dim none() { return {0.f, DimUnit::None}; }
    static constexpr dim fill() { return {100.f, DimUnit::Pct}; }
    static constexpr dim zero() { return {0.f, DimUnit::Px}; }
    static constexpr dim px(float v) { return {v, DimUnit::Px}; }
    static constexpr dim pct(float v) { return {v, DimUnit::Pct}; }

    constexpr bool operator==(const dim& rhs) const { return value == rhs.value && unit == rhs.unit; }
    constexpr bool operator!=(const dim& rhs) const { return !(*this == rhs); }
};

struct Constraint
{
    velk::aabb bounds{};
};

inline float resolve_dim(dim d, float available)
{
    switch (d.unit) {
    case DimUnit::Px:
        return d.value;
    case DimUnit::Pct:
        return d.value * available;
    default:
        return available;
    }
}

enum class HAlign : uint8_t
{
    Left,
    Center,
    Right
};

enum class VAlign : uint8_t
{
    Top,
    Center,
    Bottom
};

enum class RenderBackendType : uint8_t
{
    GL,
    Vulkan
};

struct RenderConfig
{
    RenderBackendType backend{RenderBackendType::GL};
    void* backend_params = nullptr;
};

} // namespace velk_ui

#endif // VELK_UI_TYPES_H
