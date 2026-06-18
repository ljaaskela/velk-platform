#include "android_asset_protocol.h"

#include <velk/ext/core_object.h>

#include <android/asset_manager.h>

namespace velk::impl {

namespace {

class AndroidAssetFile final : public ext::ObjectCore<AndroidAssetFile, IFile>
{
public:
    VELK_CLASS_UID(Uid{"4d6c6f49-3c4e-4d9f-aa10-7c4e5b3f9b21"}, "AndroidAssetFile");

    void init(AAssetManager* mgr, string uri, string path)
    {
        mgr_ = mgr;
        uri_ = std::move(uri);
        path_ = std::move(path);
    }

    string_view uri() const override { return uri_; }

    bool exists() const override
    {
        return open_size() >= 0;
    }

    int64_t size() const override
    {
        return open_size();
    }

    bool is_persistent() const override { return persistent_; }
    void set_persistent(bool v) override { persistent_ = v; }

    ReturnValue read(vector<uint8_t>& out) const override
    {
        AAsset* asset = AAssetManager_open(mgr_, path_.c_str(), AASSET_MODE_BUFFER);
        if (!asset) {
            return ReturnValue::Fail;
        }
        off_t len = AAsset_getLength(asset);
        out.resize(static_cast<size_t>(len));
        int n = AAsset_read(asset, out.data(), out.size());
        AAsset_close(asset);
        return n == static_cast<int>(len) ? ReturnValue::Success : ReturnValue::Fail;
    }

    ReturnValue read_text(string& out) const override
    {
        AAsset* asset = AAssetManager_open(mgr_, path_.c_str(), AASSET_MODE_BUFFER);
        if (!asset) {
            return ReturnValue::Fail;
        }
        off_t len = AAsset_getLength(asset);
        out.resize(static_cast<size_t>(len));
        int n = AAsset_read(asset, out.data(), out.size());
        AAsset_close(asset);
        return n == static_cast<int>(len) ? ReturnValue::Success : ReturnValue::Fail;
    }

    ReturnValue write(const uint8_t*, size_t) override { return ReturnValue::Fail; }
    ReturnValue write_text(string_view) override { return ReturnValue::Fail; }

private:
    int64_t open_size() const
    {
        AAsset* asset = AAssetManager_open(mgr_, path_.c_str(), AASSET_MODE_UNKNOWN);
        if (!asset) {
            return -1;
        }
        int64_t len = AAsset_getLength(asset);
        AAsset_close(asset);
        return len;
    }

    AAssetManager* mgr_{nullptr};
    string uri_;
    string path_;
    bool persistent_{false};
};

} // namespace

IResource::Ptr AndroidAssetProtocol::resolve(string_view path) const
{
    if (!mgr_) {
        return nullptr;
    }
    auto obj = ext::make_object<AndroidAssetFile>();
    // make_object<AndroidAssetFile> creates exactly that concrete type; downcast
    // is safe and the offset is known statically (no RTTI needed).
    auto* concrete = static_cast<AndroidAssetFile*>(obj.get());
    string uri{"app://"};
    uri.append(path);
    string p{path.data(), path.size()};
    concrete->init(mgr_, std::move(uri), std::move(p));
    return interface_pointer_cast<IResource>(obj);
}

} // namespace velk::impl
