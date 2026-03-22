#ifndef VELK_UI_TYPES_H
#define VELK_UI_TYPES_H

#include <velk/api/math_types.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk_ui {

enum class DrawCommandType : uint8_t
{
    FillRect,    ///< Solid color rectangle.
    TexturedQuad ///< Textured quad (glyph from atlas).
};

/**
 * @brief POD draw command produced by IVisual.
 *
 * Element-local coordinates. The renderer applies world_matrix.
 * Unused fields are zeroed (e.g. UVs for FillRect).
 */
struct DrawCommand
{
    DrawCommandType type{};
    velk::rect bounds{}; ///< Element-local position and size.
    velk::color color{}; ///< Visual color (fill or text tint).
    float u0{}, v0{};    ///< Texture UV top-left.
    float u1{}, v1{};    ///< Texture UV bottom-right.
};

enum class DirtyFlags : uint8_t
{
    None = 0,
    Layout = 1 << 0,
    Visual = 1 << 1,
    ZOrder = 1 << 2,
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

} // namespace velk_ui

#endif // VELK_UI_TYPES_H
