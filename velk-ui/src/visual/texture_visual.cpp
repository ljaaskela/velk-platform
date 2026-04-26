#include "texture_visual.h"

#include "texture_material.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/ext/core_object.h>
#include <velk/interface/intf_metadata_observer.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-scene/instance_types.h>
#include <velk-scene/interface/intf_visual.h>

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
            write_state<IVisual2D>(this, [&](IVisual2D::State& s) {
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

vector<DrawEntry> TextureVisual::get_draw_entries(::velk::IRenderContext& /*ctx*/,
                                                   const ::velk::size& bounds)
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
    entry.bounds = {0, 0, bounds.width, bounds.height};
    entry.texture_key = reinterpret_cast<uint64_t>(tex.get());
    if (auto vs2d = read_state<IVisual2D>(this); vs2d && vs2d->paint) {
        entry.material = vs2d->paint.get<IProgram>();
    }
    entry.set_instance(ElementInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {0.f, 0.f, 0.f, 0.f},
        {bounds.width, bounds.height, 0.f, 0.f},
        ::velk::color::white(),
        {0u, 0u, 0u, 0u}});

    return {entry};
}

vector<IBuffer::Ptr> TextureVisual::get_gpu_resources(::velk::IRenderContext& /*ctx*/) const
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
