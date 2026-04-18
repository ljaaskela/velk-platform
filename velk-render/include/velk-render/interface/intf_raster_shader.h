#ifndef VELK_RENDER_INTF_RASTER_SHADER_H
#define VELK_RENDER_INTF_RASTER_SHADER_H

#include <velk/interface/intf_metadata.h>
#include <velk/string_view.h>

#include <cstdint>

namespace velk {

/**
 * @brief Vertex + fragment shader source pair.
 *
 * Either field may be empty to request the registered default shader
 * for that stage at the given raster target.
 */
struct ShaderSource
{
    string_view vertex;
    string_view fragment;
};

/**
 * @brief Contract for objects that produce raster shader sources.
 *
 * Materials and visuals-with-own-shaders both implement this. The
 * `Target` enum selects which raster pass's shaders are wanted:
 * forward (swapchain-compatible) or deferred (G-buffer). Adding a
 * new raster target later (e.g. depth pre-pass, shadow map render)
 * is an enum value, not a new interface.
 */
class IRasterShader : public Interface<IRasterShader>
{
public:
    /** @brief Raster pass an `IRasterShader` is being queried for. */
    enum class Target : uint8_t
    {
        Forward,   ///< Forward raster, swapchain-compatible single target.
        Deferred,  ///< Deferred raster, G-buffer multi-attachment target.
    };

    /**
     * @brief Returns vertex + fragment source for the given target.
     *
     * Either field of the returned `ShaderSource` may be empty, in
     * which case the renderer substitutes the registered default for
     * that stage at that target.
     */
    virtual ShaderSource get_raster_source(Target t) const = 0;

    /**
     * @brief Returns a stable, class-unique 64-bit pipeline key.
     *
     * Visuals that supply their own shader use this to deduplicate
     * pipeline compilation across instances. Materials typically
     * return 0; the render context auto-assigns a key based on the
     * compiled shader identity.
     */
    virtual uint64_t get_raster_pipeline_key() const { return 0; }
};

} // namespace velk

#endif // VELK_RENDER_INTF_RASTER_SHADER_H
