#ifndef VELK_RENDER_EXT_RENDER_PATH_H
#define VELK_RENDER_EXT_RENDER_PATH_H

#include <velk/ext/core_object.h>

#include <velk-render/interface/intf_render_path.h>

namespace velk::ext {

/**
 * @brief CRTP base providing empty default impls for `IRenderPath` hooks.
 *
 * Concrete render paths inherit from this instead of `ObjectCore`
 * directly. `build_passes()` stays pure — every path must implement it.
 * Other hooks (`needs`, `on_view_removed`, `shutdown`,
 * `find_named_output`) get empty defaults so paths only override what
 * they actually produce.
 */
template <class FinalClass, class... ExtraInterfaces>
class RenderPath
    : public ObjectCore<FinalClass, ::velk::IRenderPath, ExtraInterfaces...>
{
public:
    ::velk::IRenderPath::Needs needs() const override { return {}; }

    void on_view_removed(::velk::IViewEntry& /*view*/, ::velk::FrameContext& /*ctx*/) override {}

    void shutdown(::velk::FrameContext& /*ctx*/) override {}

    ::velk::IGpuResource::Ptr find_named_output(::velk::string_view /*name*/,
                                                ::velk::IViewEntry* /*view*/) const override
    {
        return {};
    }
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_RENDER_PATH_H
