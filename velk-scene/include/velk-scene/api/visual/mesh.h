#ifndef VELK_UI_API_VISUAL_MESH_H
#define VELK_UI_API_VISUAL_MESH_H

#include <velk-scene/api/visual/visual.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Generic 3D mesh visual.
 *
 * Wraps a `MeshVisual` instance. Renders any `IMesh` set via `set_mesh`;
 * unlike `CubeVisual` / `SphereVisual` it has no procedural fallback.
 */
class MeshVisual : public Visual3D
{
public:
    MeshVisual() = default;
    explicit MeshVisual(IObject::Ptr obj) : Visual3D(std::move(obj)) {}
    explicit MeshVisual(IVisual::Ptr v) : Visual3D(as_object(v)) {}
};

namespace trait::visual {

inline MeshVisual create_mesh()
{
    return MeshVisual(instance().create<IObject>(ClassId::Visual::Mesh));
}

} // namespace trait::visual

} // namespace velk

#endif // VELK_UI_API_VISUAL_MESH_H
