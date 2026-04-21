#ifndef VELK_RENDER_STANDARD_MATERIAL_H
#define VELK_RENDER_STANDARD_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/material/intf_material_property.h>
#include <velk-render/interface/material/intf_standard_material.h>
#include <velk-render/plugin.h>

#include <velk/vector.h>

namespace velk::impl {

/**
 * @brief glTF metallic-roughness material.
 *
 * Material inputs are attached IMaterialProperty objects rather than inline
 * PROPs. `add_attachment`/`remove_attachment` track the ordered list of
 * attached properties; the effective property per class is the last attached
 * one of that class ("last wins"), so override-by-reattach is a first-class
 * authoring pattern. See design-notes/material_properties.md.
 *
 * Phase 1 reads base color and metallic/roughness factors from the active
 * BaseColorProperty / MetallicRoughnessProperty (if any). Normal, occlusion,
 * emissive, specular, alpha mode and double sided are tracked but not yet
 * consumed by the shader: that wiring lands in Phase 2.
 */
class StandardMaterial : public ::velk::ext::Material<StandardMaterial, IStandardMaterial>
{
public:
    VELK_CLASS_UID(::velk::ClassId::StandardMaterial, "StandardMaterial");

    IMaterialProperty::Ptr get_material_property(Uid class_id) const override;
    vector<IMaterialProperty::Ptr> get_material_properties() const override;

    ReturnValue add_attachment(const IInterface::Ptr& attachment) override;
    ReturnValue remove_attachment(const IInterface::Ptr& attachment) override;

    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size, ITextureResolver* resolver = nullptr) const override;

    string_view get_eval_src() const override;
    string_view get_eval_fn_name() const override;

    vector<ISurface*> get_textures() const override;

    /// Fixed slot indices into DrawDataHeader::texture_ids[] used by the
    /// standard material eval shader. Must match the order returned by
    /// get_textures() and the `velk_eval_standard` GLSL body.
    enum TextureSlot : uint32_t
    {
        SlotBaseColor = 0,
        SlotMetallicRoughness = 1,
        SlotNormal = 2,
        SlotOcclusion = 3,
        SlotEmissive = 4,
        SlotSpecular = 5,
        SlotCount = 6,
    };

private:
    using Base = ::velk::ext::Material<StandardMaterial, IStandardMaterial>;

    /// Attachments implementing IMaterialProperty, in attach order. Effective
    /// property for any class X is the last entry whose class_uid == X.
    vector<IMaterialProperty::Ptr> properties_;
};

} // namespace velk::impl

#endif // VELK_RENDER_STANDARD_MATERIAL_H
