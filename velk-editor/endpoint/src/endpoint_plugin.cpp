#include "endpoint_plugin.h"

#include <velk/api/velk.h>
#include <velk/plugins/remote/plugin.h>

#include <cstdio>
#include <cstring>

namespace velk::editor {

ReturnValue EndpointPlugin::initialize(IVelk& velk, PluginConfig&)
{
    // velk_remote must be loaded first; the runtime does that.
    server_ = velk.create<IRemoteServer>(ClassId::RemoteServer);
    if (!server_) {
        VELK_LOG(E, "editor endpoint: velk_remote is not loaded");
        return ReturnValue::Fail;
    }
    return ReturnValue::Success;
}

ReturnValue EndpointPlugin::shutdown(IVelk&)
{
    scenes_.clear();
    if (server_) {
        server_->close();
        server_ = nullptr;
    }
    return ReturnValue::Success;
}

uint16_t EndpointPlugin::listen(uint16_t port)
{
    return server_ ? server_->listen(port) : 0;
}

void EndpointPlugin::add_scene(const IScene::Ptr& scene)
{
    if (!server_ || !scene) {
        return;
    }
    for (auto& p : scenes_) {
        if (p.scene.lock() == scene) {
            return;
        }
    }
    char name[32];
    std::snprintf(name, sizeof(name), "scene%u", next_scene_++);
    scenes_.push_back({scene, string(string_view(name, std::strlen(name)))});
    server_->publish(scenes_.back().name, interface_pointer_cast<IObject>(scene));
}

void EndpointPlugin::remove_scene(const IScene::Ptr& scene)
{
    for (size_t i = 0; i < scenes_.size(); ++i) {
        if (scenes_[i].scene.lock() == scene) {
            if (server_) {
                server_->unpublish(scenes_[i].name);
            }
            scenes_.erase(scenes_.begin() + i);
            return;
        }
    }
}

} // namespace velk::editor
