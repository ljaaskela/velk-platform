#include "core_plugin.h"

#include "editor_core.h"

namespace velk::editor {

ReturnValue CorePlugin::initialize(IVelk& velk, PluginConfig&)
{
    return register_type<EditorCore>(velk);
}

ReturnValue CorePlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::editor
