#include "document.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace velk::editor {

namespace {

std::string key_of(string_view source)
{
    return std::string(source.data(), source.size());
}

} // namespace

string source_path(string_view source)
{
    constexpr string_view scheme("file://");
    if (source.size() >= scheme.size() && source.substr(0, scheme.size()) == scheme) {
        source = source.substr(scheme.size());
    }
    // file:///C:/... names a Windows drive.
    if (source.size() > 2 && source[0] == '/' && source[2] == ':') {
        source = source.substr(1);
    }
    return string(source);
}

bool Document::load_text(string_view source, string_view text, string& error)
{
    File file;
    if (!json_parse(text, file.root, error)) {
        return false;
    }
    if (!file.root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    files_[key_of(source)] = std::move(file);
    return true;
}

bool Document::load(string_view source, string& error)
{
    const string path = source_path(source);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = string("cannot read ") + path;
        return false;
    }
    string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(string_view(buf, n));
    }
    std::fclose(f);
    if (!load_text(source, text, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool Document::has(string_view source) const
{
    return files_.find(key_of(source)) != files_.end();
}

JsonValue* Document::entry(string_view source, string_view entry)
{
    auto it = files_.find(key_of(source));
    const size_t slash = entry.find("/");
    if (it == files_.end() || slash == string_view::npos) {
        return nullptr;
    }
    JsonValue* collection = it->second.root.find(entry.substr(0, slash));
    if (!collection || !collection->is_array()) {
        return nullptr;
    }
    const string_view id = entry.substr(slash + 1);
    for (auto& e : collection->as_array()) {
        auto* e_id = e.find("id");
        if (e_id && e_id->type() == JsonType::String && string_view(e_id->as_string()) == id) {
            return &e;
        }
    }
    return nullptr;
}

bool Document::has_entry(string_view source, string_view e) const
{
    return entry(source, e) != nullptr;
}

const JsonValue* Document::properties(string_view source, string_view e) const
{
    auto* found = entry(source, e);
    return found ? found->find("properties") : nullptr;
}

const JsonValue* Document::get(string_view source, string_view e, string_view name) const
{
    auto* props = properties(source, e);
    return props ? props->find(name) : nullptr;
}

bool Document::put(string_view source, string_view e, string_view name, const JsonValue* value)
{
    auto* found = entry(source, e);
    if (!found) {
        return false;
    }
    auto* props = found->find("properties");
    if (!props) {
        if (!value) {
            return true;
        }
        found->insert("properties", JsonValue::object());
        props = found->find("properties");
    }
    if (value) {
        // A value in object form ({"value": ..., "flags": ...}) keeps its other fields.
        auto* current = props->find(name);
        auto* inner = current ? current->find("value") : nullptr;
        if (inner && current->size() > 1 && !value->find("value")) {
            *inner = *value;
        } else {
            props->set(name, *value);
        }
    } else {
        props->remove(name);
        if (props->size() == 0) {
            found->remove("properties");
        }
    }
    files_[key_of(source)].modified = true;
    return true;
}

string Document::text(string_view source) const
{
    auto it = files_.find(key_of(source));
    return it != files_.end() ? json_write(it->second.root) : string();
}

bool Document::modified(string_view source) const
{
    auto it = files_.find(key_of(source));
    return it != files_.end() && it->second.modified;
}

bool Document::any_modified() const
{
    for (auto& [source, file] : files_) {
        if (file.modified) {
            return true;
        }
    }
    return false;
}

vector<string> Document::modified_sources() const
{
    vector<string> out;
    for (auto& [source, file] : files_) {
        if (file.modified) {
            out.push_back(string(string_view(source.data(), source.size())));
        }
    }
    return out;
}

bool Document::save(string_view source, string& error)
{
    auto it = files_.find(key_of(source));
    if (it == files_.end()) {
        error = string("not open: ") + source;
        return false;
    }
    const string path = source_path(source);
    const string temp = path + ".tmp";
    const string text = json_write(it->second.root);
    std::FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) {
        error = string("cannot write ") + temp;
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !written) {
        std::remove(temp.c_str());
        error = string("cannot write ") + temp;
        return false;
    }
    // Replaces the file in one step, so a failed save never leaves half a file.
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(temp.c_str()), std::filesystem::u8path(path.c_str()), ec);
    if (ec) {
        std::remove(temp.c_str());
        error = string("cannot replace ") + path;
        return false;
    }
    it->second.modified = false;
    return true;
}

} // namespace velk::editor
