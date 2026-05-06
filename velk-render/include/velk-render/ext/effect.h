#ifndef VELK_RENDER_EXT_EFFECT_H
#define VELK_RENDER_EXT_EFFECT_H

#include <velk/ext/core_object.h>

#include <velk-render/interface/intf_effect.h>

namespace velk::ext {

/**
 * @brief CRTP base providing empty default impls for `IEffect`
 *        lifecycle hooks (inherited from `IRenderStage`). `emit` stays
 *        pure — every effect must implement it.
 *
 * Use when the effect has no per-view state to release. Stateful
 * effects override `on_view_removed` / `shutdown` to clear their maps.
 */
template <class FinalClass, class... ExtraInterfaces>
class Effect
    : public ObjectCore<FinalClass, ::velk::IEffect, ExtraInterfaces...>
{
public:
    void on_view_removed(::velk::IViewEntry& /*view*/, ::velk::FrameContext& /*ctx*/) override {}

    void shutdown(::velk::FrameContext& /*ctx*/) override {}
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_EFFECT_H
