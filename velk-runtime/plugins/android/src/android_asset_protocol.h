#ifndef VELK_RUNTIME_ANDROID_ASSET_PROTOCOL_H
#define VELK_RUNTIME_ANDROID_ASSET_PROTOCOL_H

#include <velk/ext/core_object.h>
#include <velk/interface/resource/intf_resource_protocol.h>

struct AAssetManager;

namespace velk::impl {

/// IResourceProtocol that resolves `app://path` to assets bundled in the APK
/// via AAssetManager. Read-only; write returns Fail.
class AndroidAssetProtocol final
    : public ext::ObjectCore<AndroidAssetProtocol, IResourceProtocol>
{
public:
    VELK_CLASS_UID(Uid{"a3f1c7d0-67c2-44a2-9c0a-2b8b0d3e4a51"}, "AndroidAssetProtocol");

    void set_asset_manager(AAssetManager* mgr) { mgr_ = mgr; }

    string_view scheme() const override { return "app"; }
    IResource::Ptr resolve(string_view path) const override;

private:
    AAssetManager* mgr_{nullptr};
};

} // namespace velk::impl

#endif // VELK_RUNTIME_ANDROID_ASSET_PROTOCOL_H
