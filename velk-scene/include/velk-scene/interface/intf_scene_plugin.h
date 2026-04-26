#ifndef VELK_SCENE_INTF_SCENE_PLUGIN_H
#define VELK_SCENE_INTF_SCENE_PLUGIN_H

#include <velk/interface/intf_interface.h>

#include <velk-scene/interface/intf_scene.h>

namespace velk {

/**
 * @brief Plugin-level scene-services interface.
 *
 * Implemented by velk-scene's `ScenePlugin`. Exposes process-wide
 * services that don't fit on individual `IScene` instances:
 *
 * - `for_each_scene` — iterates every live scene, used by upper
 *   layers (notably velk-ui's pre-update layout pass) that need to
 *   fan out across all scenes without holding direct references.
 *
 * Future plugin-level services (scene-creation hooks, debug
 * inspection, etc.) land on the same interface — keeps the API
 * surface concentrated rather than sprouting one singleton per
 * service.
 *
 * Reach it via:
 *   auto* plugin = velk::instance().plugin_registry()
 *                       .find_plugin(PluginId::ScenePlugin).get();
 *   auto* svc = interface_cast<IScenePlugin>(plugin);
 */
class IScenePlugin : public Interface<IScenePlugin>
{
public:
    /** @brief Invokes @p cb for each currently live IScene. */
    virtual void for_each_scene(void (*cb)(IScene*, void*), void* user) = 0;

    /** @brief Internal: a Scene calls this from its constructor. */
    virtual void register_scene(IScene* scene) = 0;

    /** @brief Internal: a Scene calls this from its destructor. */
    virtual void unregister_scene(IScene* scene) = 0;
};

} // namespace velk

#endif // VELK_SCENE_INTF_SCENE_PLUGIN_H
