#ifndef VELK_UI_RAY_TRACER_H
#define VELK_UI_RAY_TRACER_H

#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

#include "view_renderer.h"

namespace velk {
class IShadowTechnique;
} // namespace velk

namespace velk::ui {

/**
 * @brief Per-view renderer that produces a compute-dispatch + blit pass.
 *
 * Owns the RT material registry (stable material ids + fill-snippet
 * metadata), the compiled compute pipeline cache (keyed by active material
 * set), and any per-view RT allocations (storage output textures live on
 * ViewEntry and are released in on_view_removed).
 *
 * Activated when a view's camera has render_path == RenderPath::RayTrace.
 * Skipped otherwise; no state is touched.
 */
class RayTracer : public IViewRenderer
{
public:
    void build_passes(ViewEntry& view,
                      const SceneState& scene_state,
                      FrameContext& ctx,
                      vector<RenderPass>& out_passes) override;
    void on_view_removed(ViewEntry& view, FrameContext& ctx) override;
    void shutdown(FrameContext& ctx) override;

private:
    struct MaterialInfo
    {
        string_view fn_name;
        // Built as fn_name + ".glsl" — owned to survive beyond the stack
        // frame that constructed it.
        string      include_name;
    };

    struct ShadowTechInfo
    {
        string_view fn_name;
        // Built as fn_name + ".glsl" — owned to survive beyond the stack
        // frame that constructed it (techniques supply only fn_name).
        string      include_name;
    };

    // Material class (low 64 bits of class UID) -> small integer id.
    // id 0 is reserved for "no material"; valid ids start at 1.
    std::unordered_map<uint64_t, uint32_t> material_id_by_class_;
    vector<MaterialInfo> material_info_by_id_;

    // Shadow-technique class (low 64 bits of class UID) -> small integer
    // id. id 0 is reserved for "no shadow technique" (fully lit).
    std::unordered_map<uint64_t, uint32_t> shadow_tech_id_by_class_;
    vector<ShadowTechInfo> shadow_tech_info_by_id_;

    // Set of pipeline keys (hashes of material-id + shadow-tech-id sets)
    // we have compiled.
    std::unordered_map<uint64_t, bool> compiled_pipelines_;

    // Scratch per-frame sets of referenced material / shadow-tech ids.
    vector<uint32_t> frame_materials_;
    vector<uint32_t> frame_shadow_techs_;

    uint32_t register_material(IProgram* prog, FrameContext& ctx);
    uint32_t register_shadow_tech(IShadowTechnique* tech, FrameContext& ctx);
    uint64_t ensure_pipeline(const vector<uint32_t>& material_ids,
                             const vector<uint32_t>& shadow_tech_ids,
                             FrameContext& ctx);
};

} // namespace velk::ui

#endif // VELK_UI_RAY_TRACER_H
