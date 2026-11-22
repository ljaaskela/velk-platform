#ifndef VELK_EDITOR_CORE_IMPL_H
#define VELK_EDITOR_CORE_IMPL_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/plugins/importer/interface/intf_importer_plugin.h>
#include <velk/plugins/remote/api/remote.h>

#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>

#include "document.h"
#include "process.h"

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
    ReturnValue fn_launch(string folder) override;
    void fn_detach() override;
    void fn_refresh() override;
    IFuture::Ptr fn_describe(uint64_t object) override;
    IFuture::Ptr fn_apply(string commands) override;
    IFuture::Ptr fn_undo() override;
    IFuture::Ptr fn_redo() override;
    IFuture::Ptr fn_capture(uint32_t window) override;

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
        vector<RemoteHandle> sourced; ///< Objects and attachments whose sources are asked for.
    };

    /** @brief Where an object was imported from. */
    struct Source
    {
        string source; ///< The file.
        string entry;  ///< "objects/<id>" and so on.
    };

    /** @brief One property change in an undo step. */
    struct Change
    {
        RemoteHandle object = 0;
        string name;
        string source;
        string entry;
        bool had_before = false; ///< The document had the property before.
        JsonValue before;
        bool has_after = false;  ///< The document has the property after.
        JsonValue after;
        RemoteValue live_before; ///< The live value before, written back on undo.
        RemoteValue live_after;
    };
    using Step = vector<Change>;

    /** @brief Runs @p step on this core when @p future resolves, unless the core or the walk is gone. */
    template <class T, class Next>
    void then(Future<T> future, Next step);

    void roots_read(const RemoteRoots& roots);
    void roots_described(const RemoteDescriptions& described);
    void next_level();
    void level_read(const RemoteChildrenList& level);
    void objects_described(const RemoteDescriptions& described);
    void sources_read(const RemoteCallResults& results);
    /** @brief Ends a tree read: the tree is current. */
    void walk_done();

    /** @brief Connects to an endpoint and starts reading its tree. */
    ReturnValue connect(string_view host, uint32_t port);
    /** @brief Writes a step's document side, before or after. */
    void put_document(const Step& step, bool after);
    /** @brief Undo or redo: moves the last step of @p from to @p to and writes its values. */
    IFuture::Ptr replay(vector<Step>& from, vector<Step>& to, bool after);
    void history_changed();

    /** @brief The node of @p object, reused from the last read when it was there. */
    IObject::Ptr node_for(RemoteHandle object, string_view label);
    void add_subtree(const IObject::Ptr& parent, RemoteHandle object);
    /** @brief Empties the tree and marks the core detached. */
    void detached();

    /** @brief The importer's JSON value codec, created on first use. */
    IJsonValueCodec* codec() const;
    /** @brief Store-file JSON of a value read from the application; "null" when it cannot be written. */
    string value_json(const RemoteValue& value) const;
    /** @brief A value of @p type read from store-file JSON, for sending to the application. */
    bool json_value(string_view json, Uid type, RemoteValue& out) const;
    string description_json(const RemoteDescription& description) const;
    const Source* source_of(RemoteHandle object) const;

    RemoteClient client_;
    ScopedHandler disconnected_;
    mutable IJsonValueCodec::Ptr codec_;
    IHierarchy::Ptr tree_;
    IObject::Ptr top_;
    std::unordered_map<RemoteHandle, IObject::Ptr> nodes_;
    std::unordered_map<RemoteHandle, IObject::Ptr> next_nodes_;
    std::unique_ptr<Walk> walk_;
    uint32_t generation_ = 0;
    RemoteHandle app_ = 0; ///< The endpoint's "app" root.
    bool editing_ = false;
    ChildProcess child_;
    Document document_;
    std::unordered_map<RemoteHandle, Source> sources_;
    std::unordered_map<RemoteHandle, Uid> classes_;
    vector<Step> undo_;
    vector<Step> redo_;
};

} // namespace velk::editor

#endif // VELK_EDITOR_CORE_IMPL_H
