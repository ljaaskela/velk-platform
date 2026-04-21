#include "image_visual.h"

#include "image_material.h"

#include <velk/api/object.h>
#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/ext/core_object.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-ui/instance_types.h>
#include <velk-ui/interface/intf_visual.h>
#include <velk-ui/plugins/image/intf_image_material.h>

namespace velk::ui::impl {

ImageVisual::ImageVisual() = default;

void ImageVisual::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    constexpr string_view names[] = {"uri", "tint"};
    if (has_state_changed<IImageVisual>(interfaceId, name, names)) {
        ensure_loaded();
    }
    invoke_visual_changed();
}

void ImageVisual::ensure_loaded()
{
    auto state = read_state<IImageVisual>(this);
    if (!state) {
        return;
    }

    // Lazily build the internal material on first call.
    if (!material_) {
        material_ = ::velk::ext::make_object<ImageMaterial>();
        if (material_) {
            // Wire material as the visual's paint so the renderer picks
            // up its pipeline.
            auto mat_ptr = interface_pointer_cast<IMaterial>(material_);
            write_state<IVisual>(this, [&](IVisual::State& s) {
                set_object_ref(s.paint, mat_ptr);
            });
        }
    }

    // Forward tint to material.
    if (material_) {
        write_state<IImageMaterial>(material_.get(), [&](IImageMaterial::State& s) {
            s.tint = state->tint;
        });
    }

    // Reload image if uri changed.
    if (string_view(loaded_uri_) != string_view(state->uri)) {
        loaded_uri_ = state->uri;
        if (loaded_uri_.empty()) {
            image_ = nullptr;
        } else {
            auto& store = ::velk::instance().resource_store();
            image_ = store.get_resource<IImage>(string_view(loaded_uri_));
        }
    }
}

vector<DrawEntry> ImageVisual::get_draw_entries(const ::velk::size& bounds)
{
    auto state = read_state<IImageVisual>(this);
    if (!state) {
        return {};
    }

    // Make sure the image / material are ready.
    ensure_loaded();

    if (!image_ || image_->status() != ImageStatus::Loaded) {
        return {};
    }

    auto* tex = interface_cast<ISurface>(image_);
    if (!tex) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = 0; // material override supplies the pipeline
    entry.bounds = {0, 0, bounds.width, bounds.height};
    entry.texture_key = reinterpret_cast<uint64_t>(tex);
    entry.set_instance(ElementInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {0.f, 0.f, 0.f, 0.f},
        {bounds.width, bounds.height, 0.f, 0.f},
        ::velk::color::white(),
        {0u, 0u, 0u, 0u}});

    return {entry};
}

vector<IBuffer::Ptr> ImageVisual::get_gpu_resources() const
{
    auto buf = interface_pointer_cast<IBuffer>(image_);
    if (!buf) {
        return {};
    }
    vector<IBuffer::Ptr> out;
    out.push_back(std::move(buf));
    return out;
}

} // namespace velk::ui::impl
