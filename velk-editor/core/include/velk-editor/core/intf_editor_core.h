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
        (RPROP, bool, editing, false),         ///< True when the application was launched for editing.
        (RPROP, bool, can_undo, false),
        (RPROP, bool, can_redo, false),
        (RPROP, bool, modified, false),        ///< True when a document has unsaved changes.
        /// Attaches to an application's editor endpoint, for inspecting: changes are not kept.
        (FN, ReturnValue, attach, (string, host), (uint32_t, port)),
        /// Starts velk_run on the app.velk in @p folder with source tracking and attaches to it, for editing.
        (FN, ReturnValue, launch, (string, folder)),
        (FN, void, detach),                    ///< Closes the connection and ends a launched application.
        (FN, void, refresh),                   ///< Reads the tree again; ignored while a read runs.
        /// Resolves to a string: the object's class, properties with store-file JSON values, and attachments, as JSON.
        (FN, IFuture::Ptr, describe, (uint64_t, object)),
        /// Applies a JSON list of commands as one undo step: {"op":"set","object":h,"name":n,"value":v}
        /// and {"op":"reset","object":h,"name":n}. Resolves to a string: {"ok":true} or {"error":"..."}.
        (FN, IFuture::Ptr, apply, (string, commands)),
        (FN, IFuture::Ptr, undo),              ///< Resolves to a ReturnValue.
        (FN, IFuture::Ptr, redo),              ///< Resolves to a ReturnValue.
        /// Writes the documents with unsaved changes to their files. Returns {"saved":[files]} or {"error":"..."}.
        (FN, string, save),
        /// Resolves to vector<uint8_t>: the application window's last frame as PNG, empty on failure.
        (FN, IFuture::Ptr, capture, (uint32_t, window))
    )
};

} // namespace velk::editor

#endif // VELK_EDITOR_INTF_EDITOR_CORE_H
