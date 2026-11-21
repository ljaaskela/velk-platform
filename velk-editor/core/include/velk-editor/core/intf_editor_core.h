#ifndef VELK_EDITOR_INTF_EDITOR_CORE_H
#define VELK_EDITOR_INTF_EDITOR_CORE_H

#include <velk/interface/intf_metadata.h>
#include <velk/plugins/remote/types.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <cstdint>

namespace velk::editor {

/** @brief One object in the attached application's tree. */
struct EditorNode
{
    RemoteHandle object = 0;
    uint32_t depth = 0; ///< 0 for a published root, 1 for a hierarchy's root object, and so on.
    string label;       ///< Root name, or class name with the classes of its attachments.
};

/**
 * @brief The editor's state for one attached application.
 *
 * Works through velk_remote and needs nothing from velk-ui, so it runs in the
 * editor application or headless. Belongs to the thread that created it;
 * events fire during instance().update().
 */
class IEditorCore : public Interface<IEditorCore>
{
public:
    VELK_INTERFACE(
        (EVT, on_attached),     ///< attach() connected.
        (EVT, on_detached),     ///< The connection closed.
        (EVT, on_tree_changed)  ///< tree() was read again.
    )

    /** @brief Attaches to an application whose editor endpoint listens on @p host and @p port. */
    virtual ReturnValue attach(string_view host, uint16_t port) = 0;

    /** @brief Closes the connection. */
    virtual void detach() = 0;

    virtual bool is_attached() const = 0;

    /** @brief Reads the application's object tree again. Ignored while a read is running. */
    virtual void refresh() = 0;

    /** @brief The tree as last read: each published root, then its hierarchy depth first. */
    virtual vector<EditorNode> tree() const = 0;
};

} // namespace velk::editor

#endif // VELK_EDITOR_INTF_EDITOR_CORE_H
