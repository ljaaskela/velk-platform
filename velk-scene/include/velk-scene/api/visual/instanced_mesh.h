#ifndef VELK_UI_API_VISUAL_INSTANCED_MESH_H
#define VELK_UI_API_VISUAL_INSTANCED_MESH_H

#include <velk-scene/api/visual/visual.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Draws meshes many times on one element. See IInstancedMeshVisual.
 */
class InstancedMeshVisual : public Visual3D
{
public:
    InstancedMeshVisual() = default;
    explicit InstancedMeshVisual(IObject::Ptr obj) : Visual3D(check_object<IInstancedMeshVisual>(obj)) {}

    /** @brief Adds a mesh instances can use and returns its index. Mesh 0 is the one from `set_mesh`. */
    uint32_t add_mesh(const IMesh::Ptr& mesh)
    {
        return with<IInstancedMeshVisual>([&](auto& v) { return v.add_mesh(mesh); });
    }

    /** @brief Adds an instance of @p mesh and returns its slot. */
    uint32_t add_instance(const mat4& transform, uint32_t mesh = 0)
    {
        return with<IInstancedMeshVisual>([&](auto& v) { return v.add_instance(transform, mesh); });
    }

    /** @brief Adds instances of @p mesh and returns the first slot. */
    uint32_t add_instances(array_view<const InstanceTransform> transforms, uint32_t mesh = 0)
    {
        return with<IInstancedMeshVisual>([&](auto& v) { return v.add_instances(transforms, mesh); });
    }

    /** @brief Moves the instance in @p slot. */
    void set_transform(uint32_t slot, const mat4& transform)
    {
        with<IInstancedMeshVisual>([&](auto& v) { v.set_transform(slot, transform); });
    }

    /** @brief Removes the instance in @p slot. */
    void remove_instance(uint32_t slot)
    {
        with<IInstancedMeshVisual>([&](auto& v) { v.remove_instance(slot); });
    }

    /** @brief Sets how far from the camera instances are drawn. 0 means no limit. */
    void set_draw_distance(float distance)
    {
        with<IInstancedMeshVisual>([&](auto& v) { v.set_draw_distance(distance); });
    }

    /** @brief Returns the number of live instances. */
    uint32_t get_instance_count() const
    {
        return with<IInstancedMeshVisual>([](auto& v) { return v.get_instance_count(); });
    }
};

namespace trait::visual {

inline InstancedMeshVisual create_instanced_mesh()
{
    return InstancedMeshVisual(instance().create<IObject>(ClassId::Visual::InstancedMesh));
}

} // namespace trait::visual

} // namespace velk

#endif // VELK_UI_API_VISUAL_INSTANCED_MESH_H
