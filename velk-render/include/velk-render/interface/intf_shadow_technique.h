#ifndef VELK_RENDER_INTF_SHADOW_TECHNIQUE_H
#define VELK_RENDER_INTF_SHADOW_TECHNIQUE_H

#include <velk-render/interface/intf_render_technique.h>

namespace velk {

class IRenderContext;
class IRenderBackend;

/**
 * @brief Per-frame, per-light context handed to shadow techniques.
 *
 * The compositor builds one of these for every light with a shadow
 * technique attached and calls `IShadowTechnique::prepare()` before the
 * lighting pass runs. RT techniques typically no-op; shadow-map-style
 * techniques use it to allocate a depth texture and queue a
 * scene-from-light-POV render pass.
 *
 * Fields filled in as the hybrid pipeline lands (MRT G-buffer, shared
 * shape buffer, light buffer, pass output vector). Kept minimal for the
 * first slice so the interface shape is concrete without committing to
 * subsystems that don't exist yet.
 */
struct ShadowContext
{
    IRenderContext* render_ctx = nullptr;  ///< For shader compilation + includes.
    IRenderBackend* backend    = nullptr;  ///< For resource allocation when needed.

    /// Which light in the frame's light buffer this invocation serves.
    /// Stable within a frame; the compositor iterates all lights and
    /// calls prepare() once per light with this index set.
    uint32_t light_index = 0;

    // TODO (when MRT lands): const GBufferView* gbuffer;
    // TODO (when the shape buffer becomes shared): const ShapeList* shape_buffer;
    // TODO (when pass queueing is plumbed): vector<RenderPass>* out_passes;
};

/**
 * @brief Pluggable shadow technique for a light.
 *
 * Attached to an ILight via RenderTrait::add_technique. The compositor
 * groups lights by technique class uid, composes a shader that
 * includes each distinct technique's snippet, and dispatches to the
 * technique-specific evaluator via a switch keyed on uid hash.
 *
 * Implied GLSL signature of `get_snippet_fn_name()`:
 *
 *     float <fn_name>(uint light_idx, vec3 world_pos, vec3 world_normal);
 *
 * Returns visibility in [0, 1]. 1.0 = fully lit, 0.0 = fully shadowed.
 * `light_idx` indexes the frame's light buffer; `world_pos` is the
 * shading point; `world_normal` is the surface normal at that point
 * (techniques use it to bias the shadow ray off the surface, avoiding
 * self-intersection).
 *
 * Concrete implementations:
 *   - `RtShadow`   : traces an occlusion ray against the shared shape
 *                    buffer. prepare() is a no-op.
 *   - `ShadowMap`  : allocates a depth texture in prepare(), queues a
 *                    render-from-light pass, projects world_pos into
 *                    the map with depth compare + PCF in the snippet.
 *                    (Not implemented in the first slice.)
 *   - `NoShadow`   : snippet returns 1.0. Effectively "disable shadows
 *                    for this light"; usually the default when no
 *                    IShadowTechnique is attached.
 */
class IShadowTechnique : public Interface<IShadowTechnique, IRenderTechnique>
{
public:
    /**
     * @brief Allocates per-frame resources and queues any render passes
     *        the technique needs before the lighting pass runs.
     *
     * RT techniques typically leave this empty. Shadow-map techniques
     * allocate a depth texture (cached by resolution) and queue the
     * scene-from-light render pass here.
     */
    virtual void prepare(ShadowContext& ctx) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADOW_TECHNIQUE_H
