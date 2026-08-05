#ifndef VELK_UI_FRAME_SNIPPET_REGISTRY_H
#define VELK_UI_FRAME_SNIPPET_REGISTRY_H

#include <velk/ext/core_object.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Concrete IFrameSnippetRegistry. Persistent per-class id maps
 *        plus per-frame active sets and cached material upload addresses.
 */
class FrameSnippetRegistry
    : public ext::ObjectCore<FrameSnippetRegistry, IFrameSnippetRegistry>
{
public:
    VELK_CLASS_UID(ClassId::FrameSnippetRegistry, "FrameSnippetRegistry");

    void begin_frame() override;

    uint32_t register_material(IProgram* prog, IRenderContext& ctx) override;
    uint32_t register_shadow_tech(IShadowTechnique* tech, IRenderContext& ctx) override;
    uint32_t register_intersect(IAnalyticShape* shape, IRenderContext& ctx) override;

    MaterialRef resolve_material(IProgram* prog, const FrameResolveContext& ctx) override;
    uint64_t resolve_data_buffer(IDrawData* dd, const FrameResolveContext& ctx) override;

    const vector<MaterialInfo>&   material_info_by_id() const override { return material_info_by_id_; }
    const vector<ShadowTechInfo>& shadow_tech_info_by_id() const override { return shadow_tech_info_by_id_; }
    const vector<IntersectInfo>&  intersect_info_by_id() const override { return intersect_info_by_id_; }
    const vector<uint32_t>&       frame_materials() const override { return frame_materials_; }
    const vector<uint32_t>&       frame_shadow_techs() const override { return frame_shadow_techs_; }
    const vector<uint32_t>&       frame_intersects() const override { return frame_intersects_; }

    const vector<IBuffer::Ptr>& frame_data_buffers() const override { return frame_data_buffers_; }

private:
    struct MaterialInstance
    {
        IProgram* prog = nullptr;
        uint32_t  mat_id = 0;
        uint32_t  mat_base = 0;
    };

    std::unordered_map<uint64_t, uint32_t> material_id_by_class_;
    vector<MaterialInfo>                   material_info_by_id_;   // 1-indexed

    std::unordered_map<uint64_t, uint32_t> shadow_tech_id_by_class_;
    vector<ShadowTechInfo>                 shadow_tech_info_by_id_; // 1-indexed

    // Intersect ids are offset so they never collide with the built-in
    // kinds (0 = rect, 1 = cube, 2 = sphere). First registered id = 3.
    std::unordered_map<uint64_t, uint32_t> intersect_id_by_class_;
    vector<IntersectInfo>                  intersect_info_by_id_;   // 1-indexed entry for id (3+i)

    // Per-frame.
    vector<MaterialInstance> frame_material_instances_;
    vector<uint32_t>         frame_materials_;
    vector<uint32_t>         frame_shadow_techs_;
    vector<uint32_t>         frame_intersects_;
    vector<IBuffer::Ptr>     frame_data_buffers_;
};

} // namespace velk

#endif // VELK_UI_FRAME_SNIPPET_REGISTRY_H
