#ifndef VELK_UI_GL_PLUGIN_IMPL_H
#define VELK_UI_GL_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

namespace velk_ui {

class GlPlugin final : public velk::ext::Plugin<GlPlugin>
{
public:
    VELK_PLUGIN_UID("e1e9e004-21cd-4cfa-b843-49b0eb358149");
    VELK_PLUGIN_NAME("velk-gl");
    VELK_PLUGIN_VERSION(0, 1, 0);

    velk::ReturnValue initialize(velk::IVelk& velk, velk::PluginConfig& config) override;
    velk::ReturnValue shutdown(velk::IVelk& velk) override;
};

} // namespace velk_ui

VELK_PLUGIN(velk_ui::GlPlugin)

#endif // VELK_UI_GL_PLUGIN_IMPL_H
