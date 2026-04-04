#ifndef VELK_RENDER_TYPES_H
#define VELK_RENDER_TYPES_H

#include <velk/api/math_types.h>

#include <cstdint>

namespace velk {

/// Well-known pipeline keys used by built-in visuals.
namespace PipelineKey {
inline constexpr uint64_t Rect = 1;
inline constexpr uint64_t Text = 2;
inline constexpr uint64_t RoundedRect = 3;
inline constexpr uint64_t Gradient = 4;
inline constexpr uint64_t CustomBase = 1000; ///< Auto-assigned keys start here.
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
    uint64_t pipeline_key{};                       ///< Which pipeline to draw with.
    uint64_t texture_key{};                        ///< Texture binding (0 = none).
    rect bounds{};                                 ///< Element-local bounds.
    uint8_t instance_data[kMaxInstanceDataSize]{}; ///< Packed instance data for the GPU.
    uint32_t instance_size{};                      ///< Bytes used in instance_data.
};

/// Selects the GPU backend.
enum class RenderBackendType : uint8_t
{
    Default, ///< Platform best: Vulkan on Windows/Linux/Android, Metal on Apple.
    Vulkan,  ///< Explicit Vulkan backend.
};

/// Configuration for creating a render context.
struct RenderConfig
{
    RenderBackendType backend = RenderBackendType::Default;
    void* backend_params = nullptr; ///< Backend-specific init params (e.g. VulkanInitParams*).
};

} // namespace velk

#endif // VELK_RENDER_TYPES_H
