#include "mesh_visual.h"

#include "primitive_shaders.h"

#include <velk/api/attachment.h>
#include <velk/api/state.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/plugin.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

vector<DrawEntry> MeshVisual::get_draw_entries(::velk::IRenderContext& /*ctx*/,
                                               const ::velk::size& /*bounds*/)
{
    auto vs3d = read_state<IVisual3D>(this);
    if (!(vs3d && vs3d->mesh)) {
        return {};
    }
    auto mesh = vs3d->mesh.get<IMesh>();
    if (!mesh) {
        return {};
    }

    auto prims = mesh->get_primitives();
    if (prims.empty()) {
        return {};
    }

    // Match the cube/sphere visuals' default 3D pipeline state. Idempotent.
    ::velk::find_or_create_attachment<::velk::IMaterialOptions>(this,
                                                                ::velk::ClassId::MaterialOptions);

    vector<DrawEntry> out;
    out.reserve(prims.size());
    for (auto& primitive : prims) {
        if (!primitive) continue;

        DrawEntry entry{};
        entry.pipeline_key = kPrimitive3DPipelineKey;
        entry.primitive = primitive;
        if (auto ps = read_state<IMeshPrimitive>(primitive.get()); ps && ps->material) {
            entry.material = ps->material.get<IProgram>();
        }

        // Imported meshes carry vertex positions in absolute local space
        // (e.g. a glTF cube spans [-0.5, 0.5] on each axis). Use unit
        // size so the shared `position * inst.size` vertex path leaves
        // the geometry unscaled; positioning + scaling come from the
        // element's Trs trait.
        ElementInstance inst{};
        inst.offset = {0.f, 0.f, 0.f, 0.f};
        inst.size = {1.f, 1.f, 1.f, 0.f};
        inst.col = ::velk::color::white();
        entry.set_instance(inst);

        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace velk::ui
