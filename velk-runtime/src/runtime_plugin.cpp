#include "runtime_plugin.h"

#include "application.h"

#include <velk/interface/resource/intf_resource.h>

#include <filesystem>
#include <json_parser.h>
#include <system_error>

namespace velk::impl {

namespace {

string generic_path(const std::filesystem::path& p)
{
    auto s = p.lexically_normal().generic_u8string();
    return string(string_view(reinterpret_cast<const char*>(s.data()), s.size()));
}

std::filesystem::path to_path(string_view s)
{
    return std::filesystem::u8path(s.begin(), s.end());
}

// @p value relative to @p folder, or as is when absolute.
string resolve(const std::filesystem::path& folder, string_view value)
{
    auto p = to_path(value);
    return generic_path(p.is_absolute() ? p : folder / p);
}

bool fail(AppConfigFile& out, string_view path, string_view message)
{
    out.error = string(path) + ": " + message;
    return false;
}

} // namespace

ReturnValue RuntimePlugin::initialize(IVelk& velk, PluginConfig&)
{
    return ::velk::register_type<Application>(velk);
}

ReturnValue RuntimePlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

bool RuntimePlugin::read_app_config(string_view path, AppConfigFile& out) const
{
    out = {};
    auto file = instance().resource_store().get_resource<IFile>(string("file://") + path);
    string text;
    if (!file || !file->exists() || failed(file->read_text(text))) {
        return fail(out, path, "cannot read the file");
    }

    JsonValue root;
    string parse_error;
    if (!json_parse(text.data(), text.size(), root, parse_error)) {
        return fail(out, path, parse_error);
    }
    auto* app = root.find("app");
    if (!app) {
        return fail(out, path, "no \"app\" object; a store file, not an application config");
    }
    if (app->type() != JsonType::Object) {
        return fail(out, path, "\"app\" is not an object");
    }
    for (auto& [key, value] : root.as_object()) {
        if (key != "app") {
            return fail(out, path, string("unknown key \"") + key + "\" next to \"app\"");
        }
    }

    std::error_code ec;
    auto folder = std::filesystem::absolute(to_path(path), ec).parent_path();
    if (ec) {
        return fail(out, path, "cannot resolve the file's folder");
    }
    out.folder = generic_path(folder);
    if (out.folder.empty() || out.folder.back() != '/') {
        out.folder.append("/");
    }
    out.app.app_root = out.folder;

    for (auto& [key, value] : app->as_object()) {
        auto string_field = [&](string& field, bool as_path) {
            if (value.type() != JsonType::String) {
                return fail(out, path, string("\"") + key + "\" is not a string");
            }
            field = as_path ? resolve(folder, value.as_string()) : value.as_string();
            return true;
        };
        if (key == "plugins") {
            if (value.type() != JsonType::Array) {
                return fail(out, path, "\"plugins\" is not an array");
            }
            for (auto& item : value.as_array()) {
                if (item.type() != JsonType::String) {
                    return fail(out, path, "\"plugins\" has an entry that is not a string");
                }
                out.app.plugins.push_back(item.as_string());
            }
        } else if (key == "store") {
            if (!string_field(out.store, true)) {
                return false;
            }
        } else if (key == "app_root") {
            if (!string_field(out.app.app_root, true)) {
                return false;
            }
            if (out.app.app_root.empty() || out.app.app_root.back() != '/') {
                out.app.app_root.append("/");
            }
        } else if (key == "assets_root") {
            if (!string_field(out.app.assets_root, true)) {
                return false;
            }
            if (out.app.assets_root.empty() || out.app.assets_root.back() != '/') {
                out.app.assets_root.append("/");
            }
        } else if (key == "backend") {
            string backend;
            if (!string_field(backend, false)) {
                return false;
            }
            if (backend == "default") {
                out.app.backend = RenderBackendType::Default;
            } else if (backend == "vulkan") {
                out.app.backend = RenderBackendType::Vulkan;
            } else {
                return fail(out, path, string("unknown backend \"") + backend + "\"");
            }
        } else if (key == "window") {
            if (value.type() != JsonType::Object) {
                return fail(out, path, "\"window\" is not an object");
            }
            for (auto& [wkey, wvalue] : value.as_object()) {
                if (wkey == "width" || wkey == "height") {
                    if (wvalue.type() != JsonType::Number || wvalue.as_number() < 1) {
                        return fail(out, path, string("window \"") + wkey + "\" is not a positive number");
                    }
                    (wkey == "width" ? out.window.width : out.window.height) = static_cast<int>(wvalue.as_number());
                } else if (wkey == "title") {
                    if (wvalue.type() != JsonType::String) {
                        return fail(out, path, "window \"title\" is not a string");
                    }
                    out.window.title = wvalue.as_string();
                } else {
                    return fail(out, path, string("unknown window key \"") + wkey + "\"");
                }
            }
        } else {
            return fail(out, path, string("unknown key \"") + key + "\" in \"app\"");
        }
    }

    if (out.store.empty()) {
        return fail(out, path, "no \"store\"");
    }
    return true;
}

} // namespace velk::impl
