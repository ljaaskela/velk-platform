#include "shader/shader_cache.h"

#include <velk/api/velk.h>
#include <velk/interface/intf_velk.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/interface/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <direct.h>
#define velk_getcwd _getcwd
#else
#include <unistd.h>
#define velk_getcwd getcwd
#endif

namespace velk {

namespace {

constexpr string_view kSchemeName = "shader_cache";

/// Portable env-var truthiness check. Treats unset, empty, and "0" as false;
/// any other value as true. Avoids MSVC's deprecation of plain getenv.
bool env_truthy(const char* name)
{
#ifdef _WIN32
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, name) != 0 || val == nullptr) {
        return false;
    }
    bool truthy = val[0] != '\0' && val[0] != '0';
    std::free(val);
    return truthy;
#else
    const char* val = std::getenv(name);
    return val && val[0] && val[0] != '0';
#endif
}

string get_cwd_with_separator()
{
    char cwd[4096];
    if (!velk_getcwd(cwd, sizeof(cwd))) {
        return {};
    }
    string base(cwd);
    if (!base.empty()) {
        char last = base.back();
        if (last != '/' && last != '\\') {
            base.append("/");
        }
    }
    return base;
}

/// Renders @p key as a 16-character lowercase hex string.
string key_to_hex(uint64_t key)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(key));
    return string(buf);
}

/// URI for the cached blob of @p key.
string blob_uri(uint64_t key)
{
    return string(kSchemeName) + "://" + key_to_hex(key) + ".spv";
}

} // namespace

void ShaderCache::ensure_initialized()
{
    if (initialized_) {
        return;
    }
    initialized_ = true;

    if (env_truthy("VELK_SHADER_CACHE_DISABLED")) {
        VELK_LOG(I, "ShaderCache: disabled via VELK_SHADER_CACHE_DISABLED");
        enabled_ = false;
        return;
    }

    // Resolve cache directory: <cwd>/shader_cache/
    string cwd = get_cwd_with_separator();
    if (cwd.empty()) {
        VELK_LOG(W, "ShaderCache: failed to determine working directory; cache disabled");
        enabled_ = false;
        return;
    }
    cache_dir_ = cwd + "shader_cache/";

    std::error_code ec;
    std::filesystem::create_directories(cache_dir_.c_str(), ec);
    if (ec) {
        VELK_LOG(W, "ShaderCache: failed to create %s: %s", cache_dir_.c_str(), ec.message().c_str());
        enabled_ = false;
        return;
    }

    // Register shader_cache:// FileProtocol.
    auto& store = instance().resource_store();
    if (!store.find_protocol(kSchemeName)) {
        auto obj = instance().create<IObject>(ClassId::FileProtocol);
        if (auto* internal = interface_cast<IResourceProtocolInternal>(obj)) {
            internal->set_scheme(kSchemeName);
            internal->set_base_path(cache_dir_);
            store.register_protocol(interface_pointer_cast<IResourceProtocol>(obj));
        } else {
            VELK_LOG(W, "ShaderCache: failed to create shader_cache:// protocol; cache disabled");
            enabled_ = false;
            return;
        }
    }
}

vector<uint32_t> ShaderCache::read(uint64_t key) const
{
    if (!enabled()) {
        return {};
    }
    auto file = instance().resource_store().get_resource<IFile>(blob_uri(key));
    if (!file || !file->exists()) {
        return {};
    }
    vector<uint8_t> bytes;
    if (failed(file->read(bytes)) || bytes.empty()) {
        return {};
    }
    if (bytes.size() % sizeof(uint32_t) != 0) {
        return {};
    }
    vector<uint32_t> spirv;
    spirv.resize(bytes.size() / sizeof(uint32_t));
    std::memcpy(spirv.data(), bytes.data(), bytes.size());
    return spirv;
}

void ShaderCache::write(uint64_t key, const vector<uint32_t>& spirv) const
{
    if (!enabled() || spirv.empty()) {
        return;
    }
    auto file = instance().resource_store().get_resource<IFile>(blob_uri(key));
    if (!file) {
        return;
    }
    auto rv = file->write(reinterpret_cast<const uint8_t*>(spirv.data()),
                          spirv.size() * sizeof(uint32_t));
    if (failed(rv)) {
        VELK_LOG(W, "ShaderCache: failed to write %s", blob_uri(key).c_str());
    }
}

} // namespace velk
