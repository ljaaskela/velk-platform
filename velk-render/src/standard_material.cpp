#include "standard_material.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

namespace velk::impl {

namespace {

VELK_GPU_STRUCT StandardParams
{
    ::velk::color base_color;
    float metallic;
    float roughness;
    float _pad[2];
};
static_assert(sizeof(StandardParams) == 32,
              "StandardParams must be 32 bytes (std430 + alignas(16))");

// Rect-instance vertex shader. Duplicates the velk-ui-provided
// rect_material_vertex_src body to avoid a velk-render -> velk-ui
// header dependency. If more velk-render materials need the same
// vertex, factor out a velk-render-owned shared constant.
constexpr string_view standard_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    OpaquePtr material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local_uv;
layout(location = 2) flat out vec2 v_size;
layout(location = 3) out vec3 v_world_pos;
layout(location = 4) out vec3 v_world_normal;
layout(location = 5) flat out uint v_shape_param;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    vec4 world_pos = inst.world_matrix * local_pos;
    gl_Position = root.global_data.view_projection * world_pos;
    v_color = inst.color;
    v_local_uv = q;
    v_size = inst.size;
    v_world_pos = world_pos.xyz;
    v_world_normal = normalize(vec3(inst.world_matrix[2]));
    v_shape_param = 0u;
}
)";

// Eval body: produce MaterialEval(Standard) carrying base_color,
// metallic, roughness + the surface normal. The framework's RT fill
// wrapper routes Standard materials through the shared
// `velk_pbr_shade` helper (direct lighting + GGX specular bounce);
// the deferred driver writes the params into the G-buffer; the
// forward driver shows base_color as-is (unlit forward preview).
constexpr string_view standard_eval_src = R"(
layout(buffer_reference, std430) readonly buffer StandardMaterialData {
    vec4 base_color;
    vec4 params;  // x = metallic, y = roughness, zw unused
};

MaterialEval velk_eval_standard(EvalContext ctx)
{
    StandardMaterialData d = StandardMaterialData(ctx.data_addr);

    MaterialEval e;
    e.color = d.base_color;
    e.normal = ctx.normal;
    e.metallic = d.params.x;
    e.roughness = d.params.y;
    e.lighting_mode = VELK_LIGHTING_STANDARD;
    return e;
}
)";

Uid property_class_id(const IMaterialProperty::Ptr& p)
{
    auto* obj = interface_cast<IObject>(p);
    return obj ? obj->get_class_uid() : Uid{};
}

} // namespace

IMaterialProperty::Ptr StandardMaterial::get_material_property(Uid class_id) const
{
    for (size_t i = properties_.size(); i > 0; --i) {
        auto& p = properties_[i - 1];
        if (p && property_class_id(p) == class_id) {
            return p;
        }
    }
    return {};
}

vector<IMaterialProperty::Ptr> StandardMaterial::get_material_properties() const
{
    // One entry per unique class id, effective (last-wins) instance only.
    vector<IMaterialProperty::Ptr> result;
    for (size_t i = properties_.size(); i > 0; --i) {
        auto& p = properties_[i - 1];
        if (!p) {
            continue;
        }
        Uid cid = property_class_id(p);
        bool already_seen = false;
        for (auto& existing : result) {
            if (property_class_id(existing) == cid) {
                already_seen = true;
                break;
            }
        }
        if (!already_seen) {
            result.push_back(p);
        }
    }
    return result;
}

ReturnValue StandardMaterial::add_attachment(const IInterface::Ptr& attachment)
{
    auto rv = Base::add_attachment(attachment);
    if (succeeded(rv)) {
        if (auto p = interface_pointer_cast<IMaterialProperty>(attachment)) {
            properties_.push_back(p);
        }
    }
    return rv;
}

ReturnValue StandardMaterial::remove_attachment(const IInterface::Ptr& attachment)
{
    auto rv = Base::remove_attachment(attachment);
    if (succeeded(rv)) {
        if (auto p = interface_pointer_cast<IMaterialProperty>(attachment)) {
            for (auto it = properties_.begin(); it != properties_.end(); ++it) {
                if (*it == p) {
                    properties_.erase(it);
                    break;
                }
            }
        }
    }
    return rv;
}

size_t StandardMaterial::get_draw_data_size() const
{
    return sizeof(StandardParams);
}

ReturnValue StandardMaterial::write_draw_data(void* out, size_t size) const
{
    color base_color{1.f, 1.f, 1.f, 1.f};
    float metallic  = 0.f;
    float roughness = 0.5f;

    // Forward pass over attach order: later entries of the same class overwrite
    // earlier ones, giving "last wins" in a single O(n) walk.
    for (auto& p : properties_) {
        if (!p) {
            continue;
        }
        Uid cid = property_class_id(p);
        if (cid == ClassId::BaseColorProperty) {
            base_color = read_state_value<IBaseColorProperty>(p, &IBaseColorProperty::State::factor);
        } else if (cid == ClassId::MetallicRoughnessProperty) {
            metallic  = read_state_value<IMetallicRoughnessProperty>(
                p, &IMetallicRoughnessProperty::State::metallic_factor);
            roughness = read_state_value<IMetallicRoughnessProperty>(
                p, &IMetallicRoughnessProperty::State::roughness_factor);
        }
    }

    return set_material<StandardParams>(out, size, [&](auto& p) {
        p.base_color = base_color;
        p.metallic   = metallic;
        p.roughness  = roughness;
    });
}

string_view StandardMaterial::get_eval_src() const
{
    return standard_eval_src;
}

string_view StandardMaterial::get_eval_fn_name() const
{
    return "velk_eval_standard";
}

string_view StandardMaterial::get_vertex_src() const
{
    return standard_vertex_src;
}

} // namespace velk::impl
