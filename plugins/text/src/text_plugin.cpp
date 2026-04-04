#include "text_plugin.h"

#include "font.h"
#include "visual/text_visual.h"

namespace velk::ui {

ReturnValue TextPlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<Font>(velk);
    rv &= register_type<TextVisual>(velk);
    return rv;
}

ReturnValue TextPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::ui
