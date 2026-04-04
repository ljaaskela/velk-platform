#ifndef VELK_RENDER_TYPES_H
#define VELK_RENDER_TYPES_H

#include <velk/api/math_types.h>

#include <cstdint>

namespace velk {

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
 * @brief Generic draw entry produced by visuals.
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
    uint64_t pipeline_key{};
    uint64_t texture_key{};
    rect bounds{};
    uint8_t instance_data[kMaxInstanceDataSize]{};
    uint32_t instance_size{};
};

enum class RenderBackendType : uint8_t
{
    Default, // Platform best: Vulkan on Windows/Linux/Android, Metal on Apple
    Vulkan,
};

struct RenderConfig
{
    RenderBackendType backend = RenderBackendType::Default;
    void* backend_params = nullptr;
};

} // namespace velk

#endif // VELK_RENDER_TYPES_H
