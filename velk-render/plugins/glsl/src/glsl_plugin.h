#ifndef VELK_RENDER_PLUGINS_GLSL_PLUGIN_IMPL_H
#define VELK_RENDER_PLUGINS_GLSL_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-render/plugin.h>
#include <velk-render/plugins/glsl/plugin.h>

namespace velk::glsl {

class GlslPlugin final : public ext::Plugin<GlslPlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::GlslCompilerPlugin);
    VELK_PLUGIN_NAME("velk-glsl");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;
};

} // namespace velk::glsl

VELK_PLUGIN(velk::glsl::GlslPlugin)

#endif // VELK_RENDER_PLUGINS_GLSL_PLUGIN_IMPL_H
