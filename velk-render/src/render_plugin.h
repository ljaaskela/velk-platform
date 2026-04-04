#ifndef VELK_RENDER_PLUGIN_IMPL_H
#define VELK_RENDER_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-render/plugin.h>

namespace velk {

class RenderPlugin final : public ext::Plugin<RenderPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::RenderPlugin);
    VELK_PLUGIN_NAME("velk_render");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;
};

} // namespace velk

VELK_PLUGIN(velk::RenderPlugin)

#endif // VELK_RENDER_PLUGIN_IMPL_H
