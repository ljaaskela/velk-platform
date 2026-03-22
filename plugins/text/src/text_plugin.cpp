#include "text_plugin.h"

#include "font.h"
#include "visual/text_visual.h"

namespace velk_ui {

velk::ReturnValue TextPlugin::initialize(velk::IVelk& velk, velk::PluginConfig&)
{
    auto rv = velk::register_type<Font>(velk);
    rv &= velk::register_type<TextVisual>(velk);
    return rv;
}

velk::ReturnValue TextPlugin::shutdown(velk::IVelk&)
{
    return velk::ReturnValue::Success;
}

} // namespace velk_ui
