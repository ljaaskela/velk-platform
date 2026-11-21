#ifndef VELK_EDITOR_CORE_PLUGIN_IMPL_H
#define VELK_EDITOR_CORE_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-editor/core/plugin.h>

namespace velk::editor {

class CorePlugin final : public ::velk::ext::Plugin<CorePlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::CorePlugin);
    VELK_PLUGIN_NAME("editor-core");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;
};

} // namespace velk::editor

VELK_PLUGIN(velk::editor::CorePlugin)

#endif // VELK_EDITOR_CORE_PLUGIN_IMPL_H
