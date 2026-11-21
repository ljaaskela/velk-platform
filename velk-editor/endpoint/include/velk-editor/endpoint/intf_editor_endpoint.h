#ifndef VELK_EDITOR_INTF_EDITOR_ENDPOINT_H
#define VELK_EDITOR_INTF_EDITOR_ENDPOINT_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk-scene/interface/intf_scene.h>

#include <cstdint>

namespace velk::editor {

/**
 * @brief Lets the editor attach to this application.
 *
 * Implemented by the endpoint plugin. The runtime starts it when the
 * application is built or started with the editor enabled, and tells it
 * about the application, its windows and the scenes it shows.
 */
class IEditorEndpoint : public Interface<IEditorEndpoint>
{
public:
    /** @brief Listens for the editor on @p port of localhost. Returns the port, or 0 on failure. */
    virtual uint16_t listen(uint16_t port) = 0;

    /** @brief The application (an IApplication), for reading back windows. */
    virtual void set_application(const IObject::Ptr& application) = 0;

    /** @brief A window the application created, numbered from 0 in creation order. */
    virtual void add_window(const IObject::Ptr& window) = 0;

    /** @brief Makes @p scene visible to the editor, as a root named "scene0", "scene1" and so on. */
    virtual void add_scene(const IScene::Ptr& scene) = 0;

    /** @brief Hides @p scene from the editor. */
    virtual void remove_scene(const IScene::Ptr& scene) = 0;
};

/**
 * @brief The application as the editor sees it: published as the root "app".
 *
 * Its functions are called through velk_remote.
 */
class IAppEndpoint : public Interface<IAppEndpoint>
{
public:
    VELK_INTERFACE(
        (RPROP, uint32_t, window_count, 0),                    ///< Windows the application created.
        (FN, vector<uint8_t>, capture, (uint32_t, window)),    ///< The window's last frame as PNG; empty on failure.
        /// Takes one object reference. Returns where the object was imported from as two strings,
        /// the source file and the entry ("objects/<id>" and so on); empty when the application
        /// does not track sources or the object was not imported.
        (FN_RAW, source)
    )
};

} // namespace velk::editor

#endif // VELK_EDITOR_INTF_EDITOR_ENDPOINT_H
