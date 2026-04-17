#include "texture_visual.h"

#include "texture_material.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/ext/core_object.h>
#include <velk/interface/intf_metadata_observer.h>
#include <velk-render/interface/intf_material.h>
#include <velk-ui/instance_types.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk::ui::impl {

void TextureVisual::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    constexpr string_view names[] = {"texture", "tint"};
    if (has_state_changed<ITextureVisual>(interfaceId, name, names)) {
        ensure_material();
    }
    invoke_visual_changed();
}

void TextureVisual::ensure_material()
{
    if (!material_) {
        material_ = ::velk::ext::make_object<TextureMaterial>();
        if (material_) {
            auto mat_ptr = interface_pointer_cast<IMaterial>(material_);
            write_state<IVisual>(this, [&](IVisual::State& s) {
                set_object_ref(s.paint, mat_ptr);
            });
        }
    }

    auto state = read_state<ITextureVisual>(this);
    if (material_ && state) {
        write_state<ITextureVisual>(material_.get(), [&](ITextureVisual::State& s) {
            s.tint = state->tint;
        });
    }
}

vector<DrawEntry> TextureVisual::get_draw_entries(const rect& bounds)
{
    ensure_material();

    auto state = read_state<ITextureVisual>(this);
    if (!state) {
        return {};
    }

    auto tex = state->texture.get<ISurface>();
    if (!tex) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = 0; // material override supplies the pipeline
    entry.bounds = bounds;
    entry.texture_key = reinterpret_cast<uint64_t>(tex.get());
    entry.set_instance(RectInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {bounds.x, bounds.y},
        {bounds.width, bounds.height},
        ::velk::color::white()});

    return {entry};
}

vector<IBuffer::Ptr> TextureVisual::get_gpu_resources() const
{
    auto state = read_state<ITextureVisual>(this);
    if (!state) {
        return {};
    }

    auto tex = state->texture.get<ISurface>();
    auto buf = interface_pointer_cast<IBuffer>(tex);
    if (!buf) {
        return {};
    }
    vector<IBuffer::Ptr> out;
    out.push_back(std::move(buf));
    return out;
}

} // namespace velk::ui::impl
