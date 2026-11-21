#ifndef VELK_EDITOR_CORE_IMPL_H
#define VELK_EDITOR_CORE_IMPL_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/plugins/remote/api/remote.h>

#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>

#include <memory>
#include <unordered_map>

namespace velk::editor {

class EditorNode final : public ::velk::ext::Object<EditorNode, IEditorNode>
{
public:
    VELK_CLASS_UID(ClassId::EditorNode, "EditorNode");
};

class EditorCore final : public ::velk::ext::Object<EditorCore, IEditorCore>
{
public:
    VELK_CLASS_UID(ClassId::EditorCore, "EditorCore");

    EditorCore();

    // IEditorCore
    ReturnValue fn_attach(string host, uint32_t port) override;
    void fn_detach() override;
    void fn_refresh() override;

private:
    /** @brief A tree read in progress: a few batched round trips, one per hierarchy level. */
    struct Walk
    {
        uint32_t generation = 0;
        vector<RemoteRoot> roots;
        vector<RemoteHandle> hierarchy_roots; ///< Per root: its hierarchy's root object, or 0.
        std::unordered_map<RemoteHandle, vector<RemoteHandle>> children;
        vector<RemoteNode> frontier;
        vector<RemoteHandle> objects;
        std::unordered_map<RemoteHandle, string> labels;
    };

    /** @brief Runs @p step on this core when @p future resolves, unless the core or the walk is gone. */
    template <class T, class Step>
    void then(Future<T> future, Step step);

    void roots_read(const RemoteRoots& roots);
    void roots_described(const RemoteDescriptions& described);
    void next_level();
    void level_read(const RemoteChildrenList& level);
    void objects_described(const RemoteDescriptions& described);

    /** @brief The node of @p object, reused from the last read when it was there. */
    IObject::Ptr node_for(RemoteHandle object, string_view label);
    void add_subtree(const IObject::Ptr& parent, RemoteHandle object);
    /** @brief Empties the tree and marks the core detached. */
    void detached();

    RemoteClient client_;
    ScopedHandler disconnected_;
    IHierarchy::Ptr tree_;
    IObject::Ptr top_;
    std::unordered_map<RemoteHandle, IObject::Ptr> nodes_;
    std::unordered_map<RemoteHandle, IObject::Ptr> next_nodes_;
    std::unique_ptr<Walk> walk_;
    uint32_t generation_ = 0;
};

} // namespace velk::editor

#endif // VELK_EDITOR_CORE_IMPL_H
