#include "core_plugin.h"

#include "editor_core.h"

namespace velk::editor {

ReturnValue CorePlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<EditorCore>(velk);
    rv &= register_type<EditorNode>(velk);
    return rv;
}

ReturnValue CorePlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::editor
