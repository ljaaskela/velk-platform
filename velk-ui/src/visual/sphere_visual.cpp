#include "sphere_visual.h"

#include "primitive_shaders.h"

#include <velk/api/state.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-ui/instance_types.h>

#include <cstring>

namespace velk::ui {

vector<DrawEntry> SphereVisual::get_draw_entries(const ::velk::size& bounds)
{
    auto vs = read_state<IVisual>(this);
    ::velk::color col = vs ? vs->color : ::velk::color::white();

    DrawEntry entry{};
    entry.pipeline_key = kPrimitive3DPipelineKey;

    ElementInstance inst{};
    inst.offset = {0.f, 0.f, 0.f, 0.f};
    inst.size = {bounds.width, bounds.height, bounds.depth, 0.f};
    inst.col = col;
    entry.set_instance(inst);

    return { entry };
}

::velk::IMesh::Ptr SphereVisual::get_mesh(::velk::IRenderContext& ctx) const
{
    auto ps = read_state<::velk::IPrimitiveShape>(this);
    uint32_t subs = ps ? ps->subdivisions : 0;
    return ctx.get_mesh_builder().get_sphere(subs);
}

::velk::ShaderSource SphereVisual::get_raster_source(::velk::IRasterShader::Target t) const
{
    if (t == ::velk::IRasterShader::Target::Forward) {
        return { primitive3d_vertex_src, primitive3d_fragment_src };
    }
    return {};
}

uint64_t SphereVisual::get_raster_pipeline_key() const
{
    return kPrimitive3DPipelineKey;
}

} // namespace velk::ui
