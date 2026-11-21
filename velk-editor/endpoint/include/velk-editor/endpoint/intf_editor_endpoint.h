#ifndef VELK_EDITOR_INTF_EDITOR_ENDPOINT_H
#define VELK_EDITOR_INTF_EDITOR_ENDPOINT_H

#include <velk/interface/intf_interface.h>

#include <velk-scene/interface/intf_scene.h>

#include <cstdint>

namespace velk::editor {

/**
 * @brief Lets the editor attach to this application.
 *
 * Implemented by the endpoint plugin. The runtime starts it when the
 * application is built or started with the editor enabled, and tells it
 * about the scenes it shows.
 */
class IEditorEndpoint : public Interface<IEditorEndpoint>
{
public:
    /** @brief Listens for the editor on @p port of localhost. Returns the port, or 0 on failure. */
    virtual uint16_t listen(uint16_t port) = 0;

    /** @brief Makes @p scene visible to the editor, as a root named "scene0", "scene1" and so on. */
    virtual void add_scene(const IScene::Ptr& scene) = 0;

    /** @brief Hides @p scene from the editor. */
    virtual void remove_scene(const IScene::Ptr& scene) = 0;
};

} // namespace velk::editor

#endif // VELK_EDITOR_INTF_EDITOR_ENDPOINT_H
