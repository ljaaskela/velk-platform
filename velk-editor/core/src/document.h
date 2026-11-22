#ifndef VELK_EDITOR_DOCUMENT_H
#define VELK_EDITOR_DOCUMENT_H

#include <velk/json.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <string>
#include <unordered_map>

namespace velk::editor {

/**
 * @brief The authored values of the files an application was launched from.
 *
 * One parsed store file per source, as the file says plus unsaved edits. A property is
 * authored when it is in its entry's "properties". Values are store-file JSON. Live values
 * never enter it.
 */
class Document
{
public:
    /** @brief Parses @p text as the file @p source. Replaces an earlier one. */
    bool load_text(string_view source, string_view text, string& error);

    /** @brief Reads and parses the file @p source names ("file://" URIs or plain paths). */
    bool load(string_view source, string& error);

    bool has(string_view source) const;

    /**
     * @brief The value of a property in an entry ("objects/<id>", "attachments/<id>", "resources/<id>").
     * @return null when the file, entry or property is not there.
     */
    const JsonValue* get(string_view source, string_view entry, string_view name) const;

    /** @brief True when the file has the entry. */
    bool has_entry(string_view source, string_view entry) const;

    /**
     * @brief Sets a property in an entry, or removes it when @p value is null.
     * @return false when the file or entry is not there.
     */
    bool put(string_view source, string_view entry, string_view name, const JsonValue* value);

    /** @brief All authored properties of an entry; null when there is none. */
    const JsonValue* properties(string_view source, string_view entry) const;

    /** @brief The file's JSON as it would be saved. */
    string text(string_view source) const;

    /** @brief True when the file changed since it was loaded. */
    bool modified(string_view source) const;

    void clear() { files_.clear(); }

private:
    struct File
    {
        JsonValue root;
        bool modified = false;
    };

    JsonValue* entry(string_view source, string_view entry);
    const JsonValue* entry(string_view source, string_view entry) const
    {
        return const_cast<Document*>(this)->entry(source, entry);
    }

    std::unordered_map<std::string, File> files_;
};

/** @brief The local path of a source: "file://" removed. */
string source_path(string_view source);

} // namespace velk::editor

#endif // VELK_EDITOR_DOCUMENT_H
