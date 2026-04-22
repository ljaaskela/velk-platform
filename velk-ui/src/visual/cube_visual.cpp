#include "cube_visual.h"

#include "primitive_shaders.h"

#include <velk/api/attachment.h>
#include <velk/api/state.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/plugin.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

vector<DrawEntry> CubeVisual::get_draw_entries(const ::velk::size& bounds)
{
    auto vs = read_state<IVisual>(this);
    ::velk::color col = vs ? vs->color : ::velk::color::white();

    DrawEntry entry{};
    entry.pipeline_key = kPrimitive3DPipelineKey;

    ElementInstance inst{};
    // world_matrix left zero; batch_builder overwrites it per instance.
    inst.offset = {0.f, 0.f, 0.f, 0.f};
    inst.size = {bounds.width, bounds.height, bounds.depth, 0.f};
    inst.col = col;
    entry.set_instance(inst);

    return { entry };
}

::velk::IMesh::Ptr CubeVisual::get_mesh(::velk::IRenderContext& ctx) const
{
    if (mesh_) {
        return mesh_;
    }
    auto ps = read_state<::velk::IPrimitiveShape>(this);
    uint32_t subs = ps ? ps->subdivisions : 0;
    mesh_ = ctx.get_mesh_builder().get_cube(subs);
    // Attach an IMaterialOptions on first use so the batch builder's
    // raster-shader path can read depth/cull/blend from it. The
    // IMaterialOptions interface defaults (LessEqual depth test, depth
    // write on, backface cull) are correct for a closed 3D mesh.
    ::velk::find_or_create_attachment<::velk::IMaterialOptions>(const_cast<CubeVisual*>(this),
                                                                ::velk::ClassId::MaterialOptions);
    return mesh_;
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

} // namespace velk::ui
