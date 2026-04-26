#ifndef VELK_UI_MESH_VISUAL_H
#define VELK_UI_MESH_VISUAL_H

#include <velk-render/interface/intf_raster_shader.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief Generic 3D mesh visual.
 *
 * Renders whatever IMesh has been set via IVisual3D::mesh. Unlike CubeVisual
 * / SphereVisual, MeshVisual has no procedural fallback: if no mesh is set
 * the visual emits no draw entries. Intended for imported assets (glTF
 * meshes, custom builders) that supply their own geometry and materials.
 *
 * Materials live on the mesh's primitives (`IMeshPrimitive::material`),
 * not on the visual; the batch builder picks them up per primitive.
 */
class MeshVisual : public ext::Visual3D<MeshVisual>
{
public:
    VELK_CLASS_UID(ClassId::Visual::Mesh, "MeshVisual");

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;
};

} // namespace velk::impl

#endif // VELK_UI_MESH_VISUAL_H
