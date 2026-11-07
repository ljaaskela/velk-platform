#ifndef VELK_UI_API_TRAIT_COMPUTE_MESH_DEFORMER_H
#define VELK_UI_API_TRAIT_COMPUTE_MESH_DEFORMER_H

#include <velk-render/interface/intf_mesh.h>
#include <velk-scene/api/trait.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Computes an element's mesh vertices on the GPU. See IComputeMeshDeformer.
 *
 * The element's visual must hold a mesh from IMeshBuilder::build_deforming.
 */
class ComputeMeshDeformer : public Trait
{
public:
    ComputeMeshDeformer() = default;
    explicit ComputeMeshDeformer(IObject::Ptr obj) : Trait(check_object<IComputeMeshDeformer>(obj)) {}

    /** @brief Sets the GLSL deform function, and how far it may move a vertex. */
    void set_deform(string_view glsl, float max_displacement)
    {
        with<IComputeMeshDeformer>([&](auto& d) { d.set_deform(glsl, max_displacement); });
    }

    /** @brief Sets the params passed to the deform function. */
    void set_params(const vec4& params)
    {
        with<IComputeMeshDeformer>([&](auto& d) { d.set_params(params); });
    }
};

namespace trait::deform {

inline ComputeMeshDeformer create_compute_mesh()
{
    return ComputeMeshDeformer(instance().create<IObject>(ClassId::Deform::ComputeMesh));
}

} // namespace trait::deform

} // namespace velk

#endif // VELK_UI_API_TRAIT_COMPUTE_MESH_DEFORMER_H
