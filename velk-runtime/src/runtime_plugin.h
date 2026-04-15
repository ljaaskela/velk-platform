#ifndef VELK_RUNTIME_PLUGIN_IMPL_H
#define VELK_RUNTIME_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-runtime/plugin.h>

namespace velk::impl {

class RuntimePlugin final : public ::velk::ext::Plugin<RuntimePlugin>
{
public:
    VELK_PLUGIN_UID(PluginId::RuntimePlugin);
    VELK_PLUGIN_NAME("velk-runtime");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;
};

} // namespace velk::impl

VELK_PLUGIN(velk::impl::RuntimePlugin)

#endif // VELK_RUNTIME_PLUGIN_IMPL_H
