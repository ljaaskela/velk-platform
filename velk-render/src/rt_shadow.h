#ifndef VELK_RENDER_RT_SHADOW_H
#define VELK_RENDER_RT_SHADOW_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Ray-traced shadow technique: traces one occlusion ray against
 *        the shared scene shape buffer.
 *
 * First-slice stub: prepare() is a no-op and the snippet returns 1.0
 * (unshadowed). Wired into the lighting pass in a later milestone
 * once the shared shape buffer and light buffer are exposed to the
 * shadow compositor — at that point the snippet becomes
 *
 *   float velk_shadow_rt(uint light_idx, vec3 world_pos) {
 *       Ray r = build_ray_to_light(light_idx, world_pos);
 *       return trace_closest_hit(r, t_max) ? 0.0 : 1.0;
 *   }
 *
 * reusing the RT path's `trace_closest_hit` + shape buffer.
 */
class RtShadow : public ::velk::ext::Object<RtShadow, IShadowTechnique>
{
public:
    VELK_CLASS_UID(ClassId::RtShadow, "RtShadow");

    string_view get_snippet_fn_name() const override;
    string_view get_snippet_source() const override;

    void prepare(ShadowContext& /*ctx*/) override {}
};

} // namespace velk::impl

#endif // VELK_RENDER_RT_SHADOW_H
