#ifndef VELK_EDITOR_ENDPOINT_PLUGIN_IMPL_H
#define VELK_EDITOR_ENDPOINT_PLUGIN_IMPL_H

#include <velk/ext/object.h>
#include <velk/ext/plugin.h>
#include <velk/plugins/remote/interface/intf_remote.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <velk-editor/endpoint/intf_editor_endpoint.h>
#include <velk-editor/endpoint/plugin.h>

namespace velk::editor {

class AppEndpoint final : public ::velk::ext::Object<AppEndpoint, IAppEndpoint>
{
public:
    VELK_CLASS_UID("8482a456-0c50-451f-9911-1108815241d5", "AppEndpoint");

    void set_application(const IObject::Ptr& application) { application_ = application; }
    void add_window(const IObject::Ptr& window);

    // IAppEndpoint
    vector<uint8_t> fn_capture(uint32_t window) override;
    IAny::Ptr fn_source(FnArgs args) override;

private:
    IObject::WeakPtr application_;
    vector<IObject::WeakPtr> windows_;
};

class EndpointPlugin final : public ::velk::ext::Plugin<EndpointPlugin, IEditorEndpoint>
{
public:
    VELK_PLUGIN_UID(PluginId::EndpointPlugin);
    VELK_PLUGIN_NAME("editor-endpoint");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;

    // IEditorEndpoint
    uint16_t listen(uint16_t port) override;
    void set_application(const IObject::Ptr& application) override;
    void add_window(const IObject::Ptr& window) override;
    void add_scene(const IScene::Ptr& scene) override;
    void remove_scene(const IScene::Ptr& scene) override;

private:
    struct Published
    {
        IScene::WeakPtr scene;
        string name;
    };

    IRemoteServer::Ptr server_;
    IObject::Ptr app_;
    vector<Published> scenes_;
    uint32_t next_scene_ = 0;
};

} // namespace velk::editor

VELK_PLUGIN(velk::editor::EndpointPlugin)

#endif // VELK_EDITOR_ENDPOINT_PLUGIN_IMPL_H
