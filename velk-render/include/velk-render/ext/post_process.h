#ifndef VELK_RENDER_EXT_POST_PROCESS_H
#define VELK_RENDER_EXT_POST_PROCESS_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_post_process.h>

namespace velk::ext {

/**
 * @brief CRTP base providing empty default impls for `IPostProcess`
 *        lifecycle hooks (inherited from `IRenderStage`). `emit` stays
 *        pure — every effect must implement it.
 *
 * Use when the effect has no per-view state to release. Concrete
 * effects with view-keyed state override `on_view_removed` /
 * `shutdown` to clear their maps.
 */
template <class FinalClass, class... ExtraInterfaces>
class PostProcess
    : public ::velk::ext::Object<FinalClass, ::velk::IPostProcess, ExtraInterfaces...>
{
public:
    void on_view_removed(::velk::IViewEntry& /*view*/, ::velk::FrameContext& /*ctx*/) override {}

    void shutdown(::velk::FrameContext& /*ctx*/) override {}
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_POST_PROCESS_H
