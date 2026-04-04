#include "render_plugin.h"
#include "render_context.h"
#include "surface.h"

namespace velk {

ReturnValue RenderPlugin::initialize(IVelk& velk, PluginConfig& config)
{
    auto rv = register_type<RenderContextImpl>(velk);
    rv &= register_type<Surface>(velk);
    return rv;
}

ReturnValue RenderPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk
