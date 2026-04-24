#include "standard_material.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_texture_resolver.h>

namespace velk::impl {

namespace {

// GPU-side per-property sub-structs. Each mirrors one IMaterialProperty
// subclass 1:1: the class-specific factor(s) plus the resolved bindless
// TextureId (0 = no texture). Padding keeps std430 16-byte alignment for
// the containing StandardMaterialParams.

VELK_GPU_STRUCT BaseColorParams
{
    ::velk::color factor{1.f, 1.f, 1.f, 1.f};  // glTF baseColorFactor (RGBA)
    uint32_t texture_id{};                      // baseColorTexture (0 = none)
    uint32_t tex_coord{};                       // 0 = ctx.uv (TEXCOORD_0), 1 = ctx.uv1
    uint32_t _pad[2]{};
};
static_assert(sizeof(BaseColorParams) == 32, "BaseColorParams must be 32 bytes");

VELK_GPU_STRUCT MetallicRoughnessParams
{
    float metallic_factor{1.f};
    float roughness_factor{1.f};
    uint32_t texture_id{};       // B = metallic, G = roughness
    uint32_t tex_coord{};        // 0 = ctx.uv, 1 = ctx.uv1
};
static_assert(sizeof(MetallicRoughnessParams) == 16, "MetallicRoughnessParams must be 16 bytes");

VELK_GPU_STRUCT NormalParams
{
    float scale{1.f};
    uint32_t texture_id{};
    uint32_t tex_coord{};        // 0 = ctx.uv, 1 = ctx.uv1
    uint32_t _pad{};
};
static_assert(sizeof(NormalParams) == 16, "NormalParams must be 16 bytes");

VELK_GPU_STRUCT OcclusionParams
{
    float strength{1.f};
    uint32_t texture_id{};       // R channel
    uint32_t tex_coord{};        // 0 = ctx.uv, 1 = ctx.uv1
    uint32_t _pad{};
};
static_assert(sizeof(OcclusionParams) == 16, "OcclusionParams must be 16 bytes");

VELK_GPU_STRUCT EmissiveParams
{
    ::velk::color factor{0.f, 0.f, 0.f, 1.f};
    float strength{1.f};         // KHR_materials_emissive_strength
    uint32_t texture_id{};
    uint32_t tex_coord{};        // 0 = ctx.uv, 1 = ctx.uv1
    uint32_t _pad{};
};
static_assert(sizeof(EmissiveParams) == 32, "EmissiveParams must be 32 bytes");

VELK_GPU_STRUCT SpecularParams
{
    ::velk::color color_factor{1.f, 1.f, 1.f, 1.f};  // KHR_materials_specular specularColorFactor
    float factor{1.f};                                 // KHR_materials_specular specularFactor
    uint32_t texture_id{};                             // specularTexture (A = specular)
    uint32_t color_texture_id{};                       // specularColorTexture (RGB = color)
    uint32_t tex_coord{};                              // UV set for specular texture
    uint32_t color_tex_coord{};                        // UV set for specular_color texture
    uint32_t _pad[2]{};
};
static_assert(sizeof(SpecularParams) == 48, "SpecularParams must be 48 bytes");

VELK_GPU_STRUCT StandardMaterialParams
{
    BaseColorParams         base_color;
    MetallicRoughnessParams metallic_roughness;
    NormalParams            normal;
    OcclusionParams         occlusion;
    EmissiveParams          emissive;
    SpecularParams          specular;
};
static_assert(sizeof(StandardMaterialParams) == 160,
              "StandardMaterialParams layout must match standard_eval_src");

// Eval: structured StandardMaterialData whose GLSL blocks mirror the
// C++ sub-structs above. Each property (base color, metallic-roughness,
// normal, occlusion, emissive, specular, alpha mode) has its factor(s)
// and texture_id contiguous so the shader reads `d.base_color.factor *
// velk_texture(d.base_color.texture_id, uv)` without indirection.
constexpr string_view standard_eval_src = R"(
struct BaseColorParams { vec4 factor; uint texture_id; uint tex_coord; uint _pad0; uint _pad1; };
struct MetallicRoughnessParams { float metallic_factor; float roughness_factor; uint texture_id; uint tex_coord; };
struct NormalParams { float scale; uint texture_id; uint tex_coord; uint _pad; };
struct OcclusionParams { float strength; uint texture_id; uint tex_coord; uint _pad; };
struct EmissiveParams { vec4 factor; float strength; uint texture_id; uint tex_coord; uint _pad; };
struct SpecularParams {
    vec4 color_factor; float factor; uint texture_id; uint color_texture_id;
    uint tex_coord; uint color_tex_coord; uint _pad0; uint _pad1;
};

layout(buffer_reference, std430) readonly buffer StandardMaterialData {
    BaseColorParams         base_color;
    MetallicRoughnessParams metallic_roughness;
    NormalParams            normal;
    OcclusionParams         occlusion;
    EmissiveParams          emissive;
    SpecularParams          specular;
};

// Per-property tex_coord selects TEXCOORD_0 (ctx.uv) or TEXCOORD_1 (ctx.uv1).
#define VELK_STD_UV(ctx, tc) ((tc) == 0u ? (ctx).uv : (ctx).uv1)

MaterialEval velk_eval_standard(EvalContext ctx)
{
    StandardMaterialData d = StandardMaterialData(ctx.data_addr);

    // Base color = factor * texture (texture defaults to white when absent).
    vec4 base = d.base_color.factor;
    if (d.base_color.texture_id != 0u) {
        base *= velk_texture(d.base_color.texture_id, VELK_STD_UV(ctx, d.base_color.tex_coord));
    }

    // MR texture: B = metallic, G = roughness per glTF spec.
    float metallic  = d.metallic_roughness.metallic_factor;
    float roughness = d.metallic_roughness.roughness_factor;
    if (d.metallic_roughness.texture_id != 0u) {
        vec4 mr = velk_texture(d.metallic_roughness.texture_id,
                               VELK_STD_UV(ctx, d.metallic_roughness.tex_coord));
        metallic  *= mr.b;
        roughness *= mr.g;
    }

    // Occlusion: R channel, lerp from 1 by strength so strength=0 disables.
    float occlusion = 1.0;
    if (d.occlusion.texture_id != 0u) {
        float ao = velk_texture(d.occlusion.texture_id,
                                VELK_STD_UV(ctx, d.occlusion.tex_coord)).r;
        occlusion = mix(1.0, ao, d.occlusion.strength);
    }

    // Emissive = factor * strength * texture.
    vec3 emissive = d.emissive.factor.rgb * d.emissive.strength;
    if (d.emissive.texture_id != 0u) {
        emissive *= velk_texture(d.emissive.texture_id,
                                 VELK_STD_UV(ctx, d.emissive.tex_coord)).rgb;
    }

    // KHR_materials_specular: factor comes from A channel, color from RGB
    // of separate textures per spec.
    float specular_factor = d.specular.factor;
    if (d.specular.texture_id != 0u) {
        specular_factor *= velk_texture(d.specular.texture_id,
                                        VELK_STD_UV(ctx, d.specular.tex_coord)).a;
    }
    vec3 specular_color = d.specular.color_factor.rgb;
    if (d.specular.color_texture_id != 0u) {
        specular_color *= velk_texture(d.specular.color_texture_id,
                                       VELK_STD_UV(ctx, d.specular.color_tex_coord)).rgb;
    }

    MaterialEval e = velk_default_material_eval();
    e.color = base;
    e.normal = ctx.normal;
    e.metallic = metallic;
    e.roughness = roughness;
    e.emissive = emissive;
    e.occlusion = occlusion;
    e.specular_color_factor = specular_color;
    e.specular_factor = specular_factor;
    e.lighting_mode = VELK_LIGHTING_STANDARD;
    return e;
}
)";

Uid property_class_id(const IMaterialProperty::Ptr& p)
{
    auto* obj = interface_cast<IObject>(p);
    return obj ? obj->get_class_uid() : Uid{};
}

ISurface* property_texture(const IMaterialProperty::Ptr& p)
{
    if (!p) return nullptr;
    auto r = read_state<IMaterialProperty>(p.get());
    return r ? r->texture.template get<ISurface>().get() : nullptr;
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
    return sizeof(StandardMaterialParams);
}

ReturnValue StandardMaterial::write_draw_data(void* out, size_t size, ITextureResolver* resolver) const
{
    auto resolve = [resolver](ISurface* s) -> uint32_t {
        return resolver ? resolver->resolve(s) : 0u;
    };

    return set_material<StandardMaterialParams>(out, size, [&](auto& p) {
        // Sub-struct defaults (in-class initializers on each *Params) match
        // the glTF 2.0 spec; set_material zero-inits + re-applies them via
        // `p = {}`, so missing properties need no explicit handling here.

        // Forward pass over attach order; later entries of the same class
        // overwrite earlier ones ("last wins" via assignment order).
        for (auto& prop : properties_) {
            if (!prop) continue;
            Uid cid = property_class_id(prop);
            ISurface* tex = property_texture(prop);

            // Common per-property base surface — every IMaterialProperty
            // carries a tex_coord PROP that selects TEXCOORD_0 (0) or
            // TEXCOORD_1 (1). Read once per property; specular has a second
            // tex_coord for its color texture and is handled below.
            uint32_t tc = 0;
            if (auto r = read_state<IMaterialProperty>(prop)) {
                tc = static_cast<uint32_t>(r->tex_coord);
            }

            if (cid == ClassId::BaseColorProperty) {
                if (auto r = read_state<IBaseColorProperty>(prop)) {
                    p.base_color.factor = r->factor;
                }
                p.base_color.texture_id = resolve(tex);
                p.base_color.tex_coord = tc;
            } else if (cid == ClassId::MetallicRoughnessProperty) {
                if (auto r = read_state<IMetallicRoughnessProperty>(prop)) {
                    p.metallic_roughness.metallic_factor = r->metallic_factor;
                    p.metallic_roughness.roughness_factor = r->roughness_factor;
                }
                p.metallic_roughness.texture_id = resolve(tex);
                p.metallic_roughness.tex_coord = tc;
            } else if (cid == ClassId::NormalProperty) {
                if (auto r = read_state<INormalProperty>(prop)) {
                    p.normal.scale = r->scale;
                }
                p.normal.texture_id = resolve(tex);
                p.normal.tex_coord = tc;
            } else if (cid == ClassId::OcclusionProperty) {
                if (auto r = read_state<IOcclusionProperty>(prop)) {
                    p.occlusion.strength = r->strength;
                }
                p.occlusion.texture_id = resolve(tex);
                p.occlusion.tex_coord = tc;
            } else if (cid == ClassId::EmissiveProperty) {
                if (auto r = read_state<IEmissiveProperty>(prop)) {
                    p.emissive.factor = r->factor;
                    p.emissive.strength = r->strength;
                }
                p.emissive.texture_id = resolve(tex);
                p.emissive.tex_coord = tc;
            } else if (cid == ClassId::SpecularProperty) {
                if (auto r = read_state<ISpecularProperty>(prop)) {
                    p.specular.factor = r->factor;
                    p.specular.color_factor = r->color_factor;
                }
                p.specular.texture_id = resolve(tex);
                p.specular.tex_coord = tc;
                // Specular_color may use a different UV set per glTF
                // KHR_materials_specular. Round 1 leaves it equal to the
                // base specular tex_coord; the importer in round 2 will
                // populate it from the spec's separate texCoord field.
                p.specular.color_tex_coord = tc;
            }
        }
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

vector<ISurface*> StandardMaterial::get_textures() const
{
    // Used by batch_builder's upload path to surface material-owned
    // textures alongside visual-owned ones. Slot order is informational;
    // write_draw_data embeds TextureIds directly in StandardMaterialParams.
    vector<ISurface*> out;
    out.reserve(SlotCount);
    out.push_back(property_texture(get_material_property(ClassId::BaseColorProperty)));
    out.push_back(property_texture(get_material_property(ClassId::MetallicRoughnessProperty)));
    out.push_back(property_texture(get_material_property(ClassId::NormalProperty)));
    out.push_back(property_texture(get_material_property(ClassId::OcclusionProperty)));
    out.push_back(property_texture(get_material_property(ClassId::EmissiveProperty)));
    out.push_back(property_texture(get_material_property(ClassId::SpecularProperty)));
    return out;
}

} // namespace velk::impl
