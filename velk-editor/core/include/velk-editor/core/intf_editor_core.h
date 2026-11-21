#ifndef VELK_EDITOR_INTF_EDITOR_CORE_H
#define VELK_EDITOR_INTF_EDITOR_CORE_H

#include <velk/api/object_ref.h>
#include <velk/interface/intf_future.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

#include <cstdint>

namespace velk::editor {

/**
 * @brief One object of the attached application, in the core's tree.
 *
 * Nodes are reused while their object exists, so a front end can keep them.
 */
class IEditorNode : public Interface<IEditorNode>
{
public:
    VELK_INTERFACE(
        (RPROP, uint64_t, object, 0), ///< The application object's RemoteHandle; 0 for the tree's top node.
        (RPROP, string, label, {})    ///< Root name, or class name with the classes of its attachments.
    )
};

/**
 * @brief The editor's state for one attached application.
 *
 * Its state is properties and its commands are functions, so front ends in
 * other processes use it through velk_remote like any object. Needs nothing
 * from velk-ui, so it also runs headless.
 */
class IEditorCore : public Interface<IEditorCore>
{
public:
    VELK_INTERFACE(
        (RPROP, bool, attached, false),        ///< True while connected to an application.
        (RPROP, string, target, {}),           ///< "host:port" of the attached application.
        (RPROP, ObjectRef, tree, {}),          ///< IHierarchy of IEditorNode: a top node, the published roots under it, their trees under them.
        (RPROP, uint32_t, revision, 0),        ///< Increases each time the tree is read again.
        (FN, ReturnValue, attach, (string, host), (uint32_t, port)), ///< Attaches to an application's editor endpoint.
        (FN, void, detach),                    ///< Closes the connection.
        (FN, void, refresh),                   ///< Reads the tree again; ignored while a read runs.
        /// Resolves to a string: the object's class, properties with store-file JSON values, and attachments, as JSON.
        (FN, IFuture::Ptr, describe, (uint64_t, object)),
        /// Sets a property from a store-file JSON value. Resolves to a ReturnValue.
        (FN, IFuture::Ptr, set, (uint64_t, object), (string, name), (string, json)),
        /// Resolves to vector<uint8_t>: the application window's last frame as PNG, empty on failure.
        (FN, IFuture::Ptr, capture, (uint32_t, window))
    )
};

} // namespace velk::editor

#endif // VELK_EDITOR_INTF_EDITOR_CORE_H
