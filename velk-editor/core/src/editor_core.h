#ifndef VELK_EDITOR_CORE_IMPL_H
#define VELK_EDITOR_CORE_IMPL_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/plugins/remote/api/remote.h>

#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>

#include <memory>
#include <unordered_map>

namespace velk::editor {

class EditorCore final : public ::velk::ext::Object<EditorCore, IEditorCore>
{
public:
    VELK_CLASS_UID(ClassId::EditorCore, "EditorCore");

    // IEditorCore
    ReturnValue attach(string_view host, uint16_t port) override;
    void detach() override;
    bool is_attached() const override;
    void refresh() override;
    vector<EditorNode> tree() const override { return tree_; }

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
    void add_subtree(RemoteHandle object, uint32_t depth, vector<EditorNode>& out) const;

    RemoteClient client_;
    ScopedHandler disconnected_;
    vector<EditorNode> tree_;
    std::unique_ptr<Walk> walk_;
    uint32_t generation_ = 0;
};

} // namespace velk::editor

#endif // VELK_EDITOR_CORE_IMPL_H
