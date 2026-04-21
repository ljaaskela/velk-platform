#ifndef VELK_RENDER_TYPES_H
#define VELK_RENDER_TYPES_H

#include <velk/api/math_types.h>

#include <velk-render/interface/intf_mesh.h>

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
 * The renderer groups entries by (pipeline_key, mesh, texture_key),
 * concatenates instance data into batches, and applies the world transform.
 *
 * Convention: the first two floats in instance_data are element-local (x, y).
 * The renderer offsets them by the element's world position.
 *
 * `mesh` is optional. When null, the renderer substitutes the engine's
 * unit-quad singleton (4 verts, 6 indices) so 2D visuals that don't yet
 * know about IMesh keep working.
 */
struct DrawEntry
{
    uint64_t pipeline_key{};                       ///< Which pipeline to draw with.
    uint64_t texture_key{};                        ///< Texture binding (0 = none).
    rect bounds{};                                 ///< Element-local bounds.
    IMesh::Ptr mesh{};                             ///< Geometry; null = unit quad.
    uint8_t instance_data[kMaxInstanceDataSize]{}; ///< Packed instance data for the GPU.
    uint32_t instance_size{};                      ///< Bytes used in instance_data.

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
};

} // namespace velk

#endif // VELK_RENDER_TYPES_H
