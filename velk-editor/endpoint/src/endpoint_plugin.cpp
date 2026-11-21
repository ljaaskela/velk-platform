#include "endpoint_plugin.h"

#include <velk/api/any.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_ref.h>
#include <velk/plugins/importer/interface/intf_importer_plugin.h>
#include <velk/plugins/importer/plugin.h>
#include <velk/plugins/remote/plugin.h>

#include <velk-render/interface/intf_image.h>
#include <velk-runtime/interface/intf_application.h>
#include <velk-ui/plugins/image/plugin.h>

#include <cstdio>
#include <cstring>

namespace velk::editor {

void AppEndpoint::add_window(const IObject::Ptr& window)
{
    windows_.push_back(window);
    write_state<IAppEndpoint>(this, [&](IAppEndpoint::State& s) {
        s.window_count = static_cast<uint32_t>(windows_.size());
    });
}

vector<uint8_t> AppEndpoint::fn_capture(uint32_t window)
{
    vector<uint8_t> png;
    auto application = interface_pointer_cast<IApplication>(application_.lock());
    auto target = window < windows_.size() ? windows_[window].lock() : nullptr;
    if (!application || !target) {
        return png;
    }
    vector<uint8_t> pixels;
    uvec2 size{};
    if (!application->read_window(target, pixels, size)) {
        return png;
    }
    auto writer = instance().create<IImageWriter>(ui::ClassId::ImageEncoder);
    if (!writer || !writer->encode_png(static_cast<int>(size.x), static_cast<int>(size.y), PixelFormat::RGBA8,
                                       pixels.data(), pixels.size(), png)) {
        VELK_LOG(E, "editor endpoint: cannot encode the capture");
        png.clear();
    }
    return png;
}

IAny::Ptr AppEndpoint::fn_source(FnArgs args)
{
    vector<string> out;
    auto* ref = args.count == 1 ? interface_cast<IObjectRef>(const_cast<IAny*>(args[0])) : nullptr;
    auto object = ref ? ref->get_object() : nullptr;
    auto plugin = instance().plugin_registry().find_plugin(::velk::PluginId::ImporterPlugin);
    auto* importer = interface_cast<IImporterPlugin>(plugin);
    if (object && importer) {
        if (auto source = importer->source_of(*object)) {
            out.push_back(source.source);
            out.push_back(source.entry);
        }
    }
    return Any<vector<string>>(out);
}

ReturnValue EndpointPlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<AppEndpoint>(velk);
    // velk_remote must be loaded first; the runtime does that.
    server_ = velk.create<IRemoteServer>(ClassId::RemoteServer);
    app_ = velk.create<IObject>(AppEndpoint::static_class_id());
    if (!server_ || !app_) {
        VELK_LOG(E, "editor endpoint: velk_remote is not loaded");
        return ReturnValue::Fail;
    }
    server_->publish("app", app_);
    return rv;
}

ReturnValue EndpointPlugin::shutdown(IVelk&)
{
    scenes_.clear();
    if (server_) {
        server_->close();
        server_ = nullptr;
    }
    app_ = nullptr;
    return ReturnValue::Success;
}

uint16_t EndpointPlugin::listen(uint16_t port)
{
    return server_ ? server_->listen(port) : 0;
}

void EndpointPlugin::set_application(const IObject::Ptr& application)
{
    if (auto* app = static_cast<AppEndpoint*>(interface_cast<IAppEndpoint>(app_))) {
        app->set_application(application);
    }
}

void EndpointPlugin::add_window(const IObject::Ptr& window)
{
    if (auto* app = static_cast<AppEndpoint*>(interface_cast<IAppEndpoint>(app_))) {
        app->add_window(window);
    }
}

void EndpointPlugin::add_scene(const IScene::Ptr& scene)
{
    if (!server_ || !scene) {
        return;
    }
    for (auto& p : scenes_) {
        if (p.scene.lock() == scene) {
            return;
        }
    }
    char name[32];
    std::snprintf(name, sizeof(name), "scene%u", next_scene_++);
    scenes_.push_back({scene, string(string_view(name, std::strlen(name)))});
    server_->publish(scenes_.back().name, interface_pointer_cast<IObject>(scene));
}

void EndpointPlugin::remove_scene(const IScene::Ptr& scene)
{
    for (size_t i = 0; i < scenes_.size(); ++i) {
        if (scenes_[i].scene.lock() == scene) {
            if (server_) {
                server_->unpublish(scenes_[i].name);
            }
            scenes_.erase(scenes_.begin() + i);
            return;
        }
    }
}

} // namespace velk::editor
