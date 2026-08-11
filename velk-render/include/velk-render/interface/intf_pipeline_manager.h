#ifndef VELK_RENDER_INTF_PIPELINE_MANAGER_H
#define VELK_RENDER_INTF_PIPELINE_MANAGER_H

#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_shader.h>
#include <velk-render/interface/intf_shader_manager.h>
#include <velk-render/render_types.h>

#include <cstdint>

namespace velk {

/**
 * @brief Cache key for compiled pipelines.
 *
 * Pipelines are uniquely identified by the tuple
 * `(user_key, target_format, depth_format, target_layout)`. The user-facing
 * API still exposes the user-key as a `uint64_t`; lookups reconstruct the
 * full key from the active path's render target description.
 *
 * - `user_key`: stable id chosen by the caller (visual / material /
 *   built-in `PipelineKey::*`) or auto-assigned when 0.
 * - `target_format`: the color attachment format the pipeline was
 *   compiled against (RGBA8, RGBA8_SRGB, RGBA16F, ...). For pipelines
 *   that don't render into a color attachment (compute, blit), use
 *   the canonical `RGBA8` placeholder so the cache key is deterministic.
 * - `depth_format`: the depth attachment format the pipeline was compiled
 *   against (`None` for no-depth targets and for compute/blit pipelines).
 *   Keyed because dynamic-rendering bakes the depth format into the
 *   pipeline, so a depth and a no-depth variant of the same shader must
 *   not share a cache slot.
 * - `target_layout`: signature of the full color-attachment layout,
 *   non-zero for MRT (G-buffer) variants and 0 for single-attachment
 *   (forward / compute). Derived from the attachment formats via
 *   `pipeline_target_layout`, so it is stable across resize / recreation
 *   of the render target. Disambiguates an MRT pipeline whose first
 *   format matches a single-attachment one (both RGBA8, say).
 */
struct PipelineCacheKey
{
    uint64_t user_key = 0;
    PixelFormat target_format = PixelFormat::RGBA8;
    DepthFormat depth_format = DepthFormat::None;
    uint64_t target_layout = 0;

    bool operator==(const PipelineCacheKey& o) const noexcept
    {
        return user_key == o.user_key
            && target_format == o.target_format
            && depth_format == o.depth_format
            && target_layout == o.target_layout;
    }
};

/**
 * @brief Stable signature of a render target's color-attachment layout.
 *
 * Single-attachment targets (forward, compute, blit) yield 0; MRT targets
 * yield a hash of their color formats. Derived from the formats only —
 * never from a render-target instance — so it is identical across resize
 * or recreation of the target, keeping pipeline-cache lookups stable.
 */
inline uint64_t pipeline_target_layout(array_view<const PixelFormat> color_formats) noexcept
{
    if (color_formats.size() <= 1) return 0;
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    for (auto f : color_formats) {
        h ^= static_cast<uint64_t>(f);
        h *= 1099511628211ull; // FNV-1a prime
    }
    // Force the top bit so an MRT layout can never equal the
    // single-attachment sentinel (0) and collide with a forward key.
    return h | 0x8000000000000000ull;
}

/**
 * @brief Compiles GPU pipelines and interns them.
 *
 * Owned by the render context and reached through
 * IRenderContext::pipelines(). Compiles shader sources through the shader
 * manager, so a pipeline and the shaders it is built from share one cache
 * story.
 *
 * The intern pool holds only weak references. Compiled pipelines are owned
 * by the recorders that bind them, so a pipeline dies when the last pass
 * referencing it does, and `find` prunes expired entries as it scans.
 */
class IPipelineManager : public Interface<IPipelineManager>
{
public:
    /** @brief Binds the backend and shader manager this compiles against. */
    virtual bool init(const IRenderBackend::Ptr& backend, IShaderManager& shaders) = 0;

    /**
     * @brief Compiles a graphics pipeline against dynamic-rendering
     *        attachment formats.
     *
     * Empty vertex or fragment source falls back to the shader manager's
     * registered default for that stage.
     *
     * @param key     Stable user key, or 0 to auto-assign one.
     * @param out_key Receives the key actually used. May be nullptr.
     * @return The strong Ptr (the pool holds a weak ref); the caller must
     *         keep it alive. nullptr on failure.
     */
    virtual IGpuPipeline::Ptr compile_dynamic(string_view fragment_source,
                                              string_view vertex_source,
                                              uint64_t key,
                                              array_view<const PixelFormat> color_formats,
                                              DepthFormat depth_format,
                                              const PipelineOptions& options = {},
                                              uint64_t* out_key = nullptr) = 0;

    /** @brief Creates a compute pipeline from an already-compiled shader. */
    virtual IGpuPipeline::Ptr create_compute(const IShader::Ptr& compute, uint64_t key = 0) = 0;

    /** @brief Convenience: compiles compute source, then creates the pipeline. */
    virtual IGpuPipeline::Ptr compile_compute(string_view compute_source, uint64_t key = 0) = 0;

    /**
     * @brief Looks up an interned pipeline by key.
     * @return nullptr if nothing has been compiled for @p key yet, or if the
     *         entry has expired.
     */
    virtual IGpuPipeline::Ptr find(const PipelineCacheKey& key) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_PIPELINE_MANAGER_H
