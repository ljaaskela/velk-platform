#ifndef VELK_UI_TYPES_H
#define VELK_UI_TYPES_H

#include <velk/api/math_types.h>

#include <cstdint>

namespace velk::ui {

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
    aabb bounds{};
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

enum class CacheMode : uint8_t
{
    None,    ///< Render directly (default).
    Cached   ///< Render subtree to texture, composite result.
};

/** @brief Entry type in the visual list stream. */
enum class VisualEntry : uint8_t
{
    Element,   ///< Regular element to draw.
    PushCache, ///< Begin cached subtree (element has cache_mode == Cached).
    PopCache   ///< End cached subtree.
};

enum class BlendMode : uint8_t
{
    Opaque,    ///< No blending, overwrite destination.
    SrcAlpha   ///< Standard alpha blending: srcAlpha, oneMinusSrcAlpha (default).
};

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

enum class TextLayout : uint8_t
{
    SingleLine, ///< Everything on one line; ellipsis truncation at bounds width.
    MultiLine,  ///< Respects \n line breaks.
    WordWrap    ///< Wraps at bounds width at word boundaries; also respects \n.
};

} // namespace velk::ui

#endif // VELK_UI_TYPES_H
