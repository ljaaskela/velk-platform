#include "shader_material.h"

#include <velk/api/object.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_metadata.h>

#include <cstring>

namespace velk::impl {

ReturnValue ShaderMaterial::setup_inputs(const vector<ShaderParam>& params)
{
    params_ = params;

    gpu_data_size_ = 0;
    for (auto& p : params_) {
        size_t end = p.offset + p.size;
        if (end > gpu_data_size_) {
            gpu_data_size_ = end;
        }
    }
    gpu_data_size_ = (gpu_data_size_ + 15) & ~size_t(15);

    auto state = write_state<IShaderMaterial>(this);
    if (!state) {
        return ReturnValue::Fail;
    }
    if (!state->inputs) {
        // No input object preset, create a new object for shader input storage
        state->inputs.set(::velk::create_object());
        state->inputs.set_owning(true);
    }
    if (auto* meta = interface_cast<IMetadata>(state->inputs.get())) {
        // Go through params and add them as a property
        for (auto& p : params_) {
            auto existing = meta->get_property(string_view(p.name.c_str(), p.name.size()));
            if (!existing) {
                meta->add_property(string_view(p.name.c_str(), p.name.size()), p.type_uid, nullptr);
            }
        }
    }

    return ReturnValue::Success;
}

size_t ShaderMaterial::get_draw_data_size() const
{
    return gpu_data_size_;
}

ReturnValue ShaderMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    if (params_.empty() || !out || size < gpu_data_size_) {
        return ReturnValue::Fail;
    }
    std::memset(out, 0, size);
    auto state = read_state<IShaderMaterial>(this);
    if (!(state && state->inputs)) {
        return ReturnValue::Fail;
    }
    auto* meta = interface_cast<IMetadata>(state->inputs.get());
    if (!meta) {
        return ReturnValue::Fail;
    }
    auto* dst = static_cast<uint8_t*>(out);
    for (auto& p : params_) {
        if (p.offset + p.size <= size) {
            if (auto prop = meta->get_property(p.name)) {
                if (auto val = prop->get_value()) {
                    val->get_data(dst + p.offset, p.size, p.type_uid);
                }
            }
        }
    }
    return ReturnValue::Success;
}

} // namespace velk::impl
