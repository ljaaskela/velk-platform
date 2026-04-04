#ifndef VELK_UI_INTF_SCENE_OBSERVER_H
#define VELK_UI_INTF_SCENE_OBSERVER_H

#include <velk/interface/intf_interface.h>

#include <velk-ui/interface/intf_scene.h>

namespace velk::ui {

/**
 * @brief Callback interface for elements that need to know when they enter or leave a scene.
 *
 * Element implements this instead of IHierarchyAware. When Scene adds or removes
 * an element, it calls on_attached/on_detached. The element stores the IScene*
 * for dirty notifications.
 */
class ISceneObserver : public Interface<ISceneObserver>
{
public:
    /** @brief Called when the element is added to a scene. */
    virtual void on_attached(IScene& scene) = 0;

    /** @brief Called when the element is removed from a scene. */
    virtual void on_detached(IScene& scene) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_SCENE_OBSERVER_H
