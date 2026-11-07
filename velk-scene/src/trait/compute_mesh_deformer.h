#ifndef VELK_SCENE_COMPUTE_MESH_DEFORMER_H
#define VELK_SCENE_COMPUTE_MESH_DEFORMER_H

#include <velk/string.h>

#include <velk-scene/ext/trait.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief Trait holding what a mesh's vertices are computed from on the GPU. The element's
 *        visual must hold a mesh from IMeshBuilder::build_deforming.
 */
class ComputeMeshDeformer : public ext::Trait<ComputeMeshDeformer, ITrait, IComputeMeshDeformer>
{
public:
    VELK_CLASS_UID(ClassId::Deform::ComputeMesh, "ComputeMeshDeformer");

    TraitPhase get_phase() const override { return TraitPhase::Visual; }

    void set_deform(string_view glsl, float max_displacement) override
    {
        source_ = string(glsl);
        max_displacement_ = max_displacement;
        invoke_trait_dirty(DirtyFlags::Deform);
    }

    void set_params(const vec4& params) override
    {
        params_ = params;
        invoke_trait_dirty(DirtyFlags::Deform);
    }

    string_view get_deform_source() const override { return source_; }
    float get_max_displacement() const override { return max_displacement_; }
    vec4 get_params() const override { return params_; }

protected:
    void on_state_changed(string_view, IMetadata&, Uid) override {}

private:
    string source_;
    float max_displacement_ = 0.f;
    vec4 params_{};
};

} // namespace velk::impl

#endif // VELK_SCENE_COMPUTE_MESH_DEFORMER_H
