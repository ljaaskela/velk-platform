#include "cube_visual.h"

#include "primitive_shaders.h"

#include <velk/api/attachment.h>
#include <velk/api/state.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/plugin.h>
#include <velk-scene/instance_types.h>

namespace velk::impl {

vector<DrawEntry> CubeVisual::get_draw_entries(::velk::IRenderContext& ctx,
                                               const ::velk::size& bounds)
{
    auto ps = read_state<::velk::IPrimitiveShape>(this);
    uint32_t subs = ps ? ps->subdivisions : 0;

    // Use an author-set mesh if present; otherwise lazy-allocate a
    // fresh procedural cube and publish it into IVisual3D::mesh so the
    // caller can reach the primitive (and set per-instance material).
    IMesh::Ptr mesh;
    if (auto vs3d = read_state<IVisual3D>(this); vs3d && vs3d->mesh) {
        mesh = vs3d->mesh.get<IMesh>();
    }
    if (!mesh) {
        mesh = ctx.get_mesh_builder().get_cube(subs);
        if (mesh) {
            write_state<IVisual3D>(this, [&](IVisual3D::State& s) {
                set_object_ref(s.mesh, mesh);
            });
        }
    }
    IMeshPrimitive::Ptr primitive;
    if (mesh) {
        auto prims = mesh->get_primitives();
        if (prims.size() > 0) primitive = prims[0];
    }

    // Attach an IMaterialOptions so the batch builder's raster-shader
    // path reads depth/cull/blend for a closed 3D mesh. find_or_create
    // is idempotent.
    ::velk::find_or_create_attachment<::velk::IMaterialOptions>(this,
                                                                ::velk::ClassId::MaterialOptions);

    DrawEntry entry{};
    entry.pipeline_key = kPrimitive3DPipelineKey;
    entry.primitive = primitive;
    if (primitive) {
        if (auto ps = read_state<IMeshPrimitive>(primitive.get()); ps && ps->material) {
            entry.material = ps->material.get<IProgram>();
        }
    }

    ElementInstance inst{};
    // world_matrix left zero; batch_builder overwrites it per instance.
    inst.offset = {0.f, 0.f, 0.f, 0.f};
    inst.size = {bounds.width, bounds.height, bounds.depth, 0.f};
    inst.col = ::velk::color::white();
    entry.set_instance(inst);

    return { entry };
}

::velk::ShaderSource CubeVisual::get_raster_source(::velk::IRasterShader::Target t) const
{
    if (t == ::velk::IRasterShader::Target::Forward) {
        return { primitive3d_vertex_src, primitive3d_fragment_src };
    }
    return {};
}

uint64_t CubeVisual::get_raster_pipeline_key() const
{
    return kPrimitive3DPipelineKey;
}

} // namespace velk::impl
