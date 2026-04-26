#ifndef VELK_SCENE_PLUGIN_IMPL_H
#define VELK_SCENE_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>
#include <velk/vector.h>

#include <velk-scene/interface/intf_scene_plugin.h>
#include <velk-scene/plugin.h>

namespace velk {

class ScenePlugin final : public ::velk::ext::Plugin<ScenePlugin, ::velk::IScenePlugin>
{
public:
    VELK_PLUGIN_UID(::velk::PluginId::ScenePlugin);
    VELK_PLUGIN_NAME("velk-scene");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ::velk::ReturnValue initialize(::velk::IVelk& velk, ::velk::PluginConfig& config) override;
    ::velk::ReturnValue shutdown(::velk::IVelk& velk) override;

    // IScenePlugin
    void for_each_scene(void (*cb)(::velk::IScene*, void*), void* user) override
    {
        if (!cb) return;
        for (auto* s : scenes_) cb(s, user);
    }

    void register_scene(::velk::IScene* scene) override
    {
        if (scene) scenes_.push_back(scene);
    }

    void unregister_scene(::velk::IScene* scene) override
    {
        for (size_t i = 0; i < scenes_.size(); ++i) {
            if (scenes_[i] == scene) {
                scenes_[i] = scenes_.back();
                scenes_.pop_back();
                return;
            }
        }
    }

private:
    ::velk::vector<::velk::IScene*> scenes_;
};

} // namespace velk

VELK_PLUGIN(::velk::ScenePlugin)

#endif // VELK_SCENE_PLUGIN_IMPL_H
