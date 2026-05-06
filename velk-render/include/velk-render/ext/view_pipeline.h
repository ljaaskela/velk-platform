#ifndef VELK_RENDER_EXT_VIEW_PIPELINE_H
#define VELK_RENDER_EXT_VIEW_PIPELINE_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_view_pipeline.h>

namespace velk::ext {

/**
 * @brief CRTP base providing empty default impls for `IViewPipeline` hooks.
 *
 * Use when a pipeline implementation has no per-view state to release
 * and wants to inherit `needs() = {}`, `on_view_removed = {}`, and
 * `shutdown = {}` without writing the empty bodies. `emit()` stays
 * pure — every pipeline must implement it.
 *
 * Inherits the full `ObjectCore<FinalClass, IViewPipeline>` chain;
 * concrete impls inherit from this instead of `ObjectCore` directly.
 */
template <class FinalClass, class... ExtraInterfaces>
class ViewPipeline
    : public ::velk::ext::Object<FinalClass, ::velk::IViewPipeline, ExtraInterfaces...>
{
public:
    ::velk::IRenderPath::Needs needs(const ::velk::FrameContext& /*ctx*/) const override
    {
        return {};
    }

    void on_view_removed(::velk::IViewEntry& /*view*/, ::velk::FrameContext& /*ctx*/) override {}

    void shutdown(::velk::FrameContext& /*ctx*/) override {}
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_VIEW_PIPELINE_H
