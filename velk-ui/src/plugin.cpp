#include "plugin.h"

namespace velk_ui {

velk::ReturnValue VelkUiPlugin::initialize(velk::IVelk& velk, velk::PluginConfig& config)
{
    config.enableUpdate = true;

    auto rv = velk::register_type<Element>(velk);
    rv &= velk::register_type<Scene>(velk);
    rv &= velk::register_type<Stack>(velk);
    rv &= velk::register_type<FixedSize>(velk);
    rv &= velk::register_type<ConstraintImportHandler>(velk);
    rv &= velk::register_type<RectVisual>(velk);
    rv &= velk::register_type<VisualImportHandler>(velk);
    return rv;
}

velk::ReturnValue VelkUiPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

void VelkUiPlugin::post_update(const IPlugin::PostUpdateInfo& info)
{
    for (auto* scene : Scene::live_scenes()) {
        scene->update(info.info);
    }
}

} // namespace velk_ui
