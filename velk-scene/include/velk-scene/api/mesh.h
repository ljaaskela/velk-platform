#ifndef VELK_UI_API_MESH_H
#define VELK_UI_API_MESH_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/material/intf_material.h>

namespace velk {

/**
 * @brief API wrapper around an IMeshPrimitive.
 *
 * Provides typed material access. Geometry fields are immutable once
 * built; the wrapper exposes only the per-instance-mutable material slot.
 */
class MeshPrimitive : public Object
{
public:
    MeshPrimitive() = default;
    explicit MeshPrimitive(IObject::Ptr obj) : Object(check_object<IMeshPrimitive>(obj)) {}
    explicit MeshPrimitive(IMeshPrimitive::Ptr p) : Object(as_object(p)) {}

    operator IMeshPrimitive::Ptr() const { return as_ptr<IMeshPrimitive>(); }

    /** @brief Returns the primitive's material, or empty. */
    IMaterial::Ptr get_material() const
    {
        auto ref = read_state_value<IMeshPrimitive>(&IMeshPrimitive::State::material);
        return ref.template get<IMaterial>();
    }

    /** @brief Sets the primitive's material. */
    void set_material(const IMaterial::Ptr& mat)
    {
        write_state<IMeshPrimitive>([&](IMeshPrimitive::State& s) {
            set_object_ref(s.material, mat);
        });
    }
};

/**
 * @brief API wrapper around an IMesh container.
 *
 * Exposes the primitive list via typed accessors and offers an
 * `set_material(idx, mat)` convenience that forwards to the indexed
 * primitive.
 */
class Mesh : public Object
{
public:
    Mesh() = default;
    explicit Mesh(IObject::Ptr obj) : Object(check_object<IMesh>(obj)) {}
    explicit Mesh(IMesh::Ptr m) : Object(as_object(m)) {}

    operator IMesh::Ptr() const { return as_ptr<IMesh>(); }

    /** @brief Returns the number of primitives. */
    size_t primitive_count() const
    {
        auto m = as_ptr<IMesh>();
        return m ? m->get_primitives().size() : 0;
    }

    /** @brief Returns the primitive at @p idx, or an empty wrapper if out of range. */
    MeshPrimitive primitive(size_t idx) const
    {
        auto m = as_ptr<IMesh>();
        if (!m) return {};
        auto prims = m->get_primitives();
        if (idx >= prims.size()) return {};
        return MeshPrimitive(prims[idx]);
    }

    /** @brief Convenience: set material on the primitive at @p idx. */
    void set_material(size_t idx, const IMaterial::Ptr& mat)
    {
        primitive(idx).set_material(mat);
    }
};

} // namespace velk

#endif // VELK_UI_API_MESH_H
