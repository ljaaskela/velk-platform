#ifndef VELK_RENDER_STANDARD_MATERIAL_H
#define VELK_RENDER_STANDARD_MATERIAL_H

#include <velk-render/ext/material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_standard.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief glTF metallic-roughness material.
 *
 * Migrated to the eval-driver architecture. The eval body produces a
 * MaterialEval with lighting_mode=Standard; the framework's shared
 * `velk_pbr_shade` helper handles direct lighting + GGX specular
 * sampling in the RT path. Deferred writes PBR params into the
 * G-buffer; forward shows base_color unlit (forward path doesn't do
 * PBR lighting — that lives in deferred / RT).
 */
class StandardMaterial : public ::velk::ext::Material<StandardMaterial, IStandard>
{
public:
    VELK_CLASS_UID(::velk::ClassId::StandardMaterial, "StandardMaterial");

    size_t get_draw_data_size() const override;
    ReturnValue write_draw_data(void* out, size_t size) const override;

    string_view get_eval_src() const override;
    string_view get_eval_fn_name() const override;
    string_view get_vertex_src() const override;
};

} // namespace velk::impl

#endif // VELK_RENDER_STANDARD_MATERIAL_H
