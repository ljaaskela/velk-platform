#ifndef VELK_UI_API_VISUAL_H
#define VELK_UI_API_VISUAL_H

#include <velk/api/state.h>

#include <velk-render/interface/material/intf_material.h>
#include <velk-scene/api/mesh.h>
#include <velk-scene/api/trait.h>
#include <velk-scene/interface/intf_visual.h>

namespace velk {

/**
 * @brief Base API wrapper for all visuals.
 *
 * Wraps any object implementing IVisual. Carries no 2D- or 3D-specific
 * accessors — those live on `Visual2D` / `Visual3D`.
 */
class Visual : public Trait
{
public:
    /** @brief Default-constructed Visual wraps no object. */
    Visual() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IVisual. */
    explicit Visual(IObject::Ptr obj) : Trait(check_object<IVisual>(obj)) {}

    /** @brief Wraps an existing IVisual pointer. */
    explicit Visual(IVisual::Ptr v) : Trait(as_object(v)) {}

    /** @brief Implicit conversion to IVisual::Ptr. */
    operator IVisual::Ptr() const { return as_ptr<IVisual>(); }
};

/**
 * @brief Visual2D API wrapper — 2D authoring affordances (color, paint,
 *        visual phase).
 *
 * Inherited by every 2D visual wrapper (RectVisual, RoundedRectVisual,
 * TextVisual, ImageVisual, TextureVisual). 2D visuals draw into the
 * element's layout box with a fill (color or paint material).
 */
class Visual2D : public Visual
{
public:
    Visual2D() = default;
    explicit Visual2D(IObject::Ptr obj) : Visual(check_object<IVisual2D>(obj)) {}
    explicit Visual2D(IVisual::Ptr v) : Visual(v) {}

    /** @brief Returns the base color. */
    auto get_color() const { return read_state_value<IVisual2D>(&IVisual2D::State::color); }

    /** @brief Sets the base color. Used when no paint is set. */
    void set_color(const color& v) { write_state_value<IVisual2D>(&IVisual2D::State::color, v); }

    /** @brief Returns the paint (IMaterial reference), or empty. */
    auto get_paint() const { return read_state_value<IVisual2D>(&IVisual2D::State::paint); }

    /** @brief Sets a material as the paint, overriding the default color. */
    void set_paint(const IMaterial::Ptr& material)
    {
        write_state<IVisual2D>([&](IVisual2D::State& s) { set_object_ref(s.paint, material); });
    }

    /** @brief Returns the visual phase. */
    auto get_visual_phase() const { return read_state_value<IVisual2D>(&IVisual2D::State::visual_phase); }
};

/**
 * @brief Visual3D API wrapper — 3D authoring affordances (mesh reference).
 *
 * Inherited by every 3D visual wrapper (CubeVisual, SphereVisual, future
 * mesh / glTF visuals). Materials live on the mesh's primitives; reach
 * them via `get_mesh().primitive(idx).set_material(mat)` or the
 * convenience `get_mesh().set_material(idx, mat)`.
 *
 * Procedural primitives lazily populate their mesh on first render when
 * the caller hasn't set one explicitly, so `get_mesh()` can return
 * empty before the first frame.
 */
class Visual3D : public Visual
{
public:
    Visual3D() = default;
    explicit Visual3D(IObject::Ptr obj) : Visual(check_object<IVisual3D>(obj)) {}
    explicit Visual3D(IVisual::Ptr v) : Visual(v) {}

    /** @brief Returns the authored mesh reference (may be empty before
     *         the first render on procedural primitives). */
    Mesh get_mesh() const
    {
        auto ref = read_state_value<IVisual3D>(&IVisual3D::State::mesh);
        return Mesh(ref.get<IMesh>());
    }

    /** @brief Sets the mesh the visual draws. */
    void set_mesh(const IMesh::Ptr& mesh)
    {
        write_state<IVisual3D>([&](IVisual3D::State& s) { set_object_ref(s.mesh, mesh); });
    }
};

} // namespace velk

#endif // VELK_UI_API_VISUAL_H
