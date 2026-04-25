#ifndef VELK_RENDER_API_SHADOW_TECHNIQUE_H
#define VELK_RENDER_API_SHADOW_TECHNIQUE_H

#include <velk-render/api/render_technique.h>
#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Typed wrapper around an IShadowTechnique object.
 *
 * Concrete technique wrappers (RtShadow, ShadowMap, NoShadow) inherit
 * this and add any parameter accessors specific to the technique.
 * Accepted by `RenderTrait::add_technique` via the `RenderTechnique`
 * base.
 */
class ShadowTechnique : public RenderTechnique
{
public:
    ShadowTechnique() = default;

    explicit ShadowTechnique(IObject::Ptr obj)
        : RenderTechnique(check_object<IShadowTechnique>(obj)) {}

    explicit ShadowTechnique(IShadowTechnique::Ptr t)
        : RenderTechnique(as_object(t)) {}

    operator IShadowTechnique::Ptr() const { return as_ptr<IShadowTechnique>(); }
};

namespace technique {

/**
 * @brief Creates a ray-traced shadow technique.
 *
 * Attach to a Light via `light.add_technique(create_rt_shadow())`.
 * First-slice stub (snippet returns 1.0) until the lighting pass
 * lands; once the compositor consumes IShadowTechnique evaluators,
 * this will trace one occlusion ray per shading point.
 */
inline ShadowTechnique create_rt_shadow()
{
    return ShadowTechnique(instance().create<IObject>(ClassId::RtShadow));
}

} // namespace technique

} // namespace velk

#endif // VELK_RENDER_API_SHADOW_TECHNIQUE_H
