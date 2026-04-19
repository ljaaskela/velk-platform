#ifndef VELK_UI_FRAME_SNIPPET_REGISTRY_H
#define VELK_UI_FRAME_SNIPPET_REGISTRY_H

#include <velk/string.h>
#include <velk/vector.h>

#include <unordered_map>

namespace velk {
class IProgram;
class IShadowTechnique;
class IAnalyticShape;
class IRenderContext;
} // namespace velk

namespace velk::ui {

class FrameDataManager;

/**
 * @brief Scene-wide registry of composed shader snippets.
 *
 * Tracks stable small-integer ids for each material (IProgram +
 * IShaderSnippet) and shadow technique (IShadowTechnique) the renderer
 * has seen, plus which of those ids are "active" in the current frame.
 * Composer-based pipelines (RT compute) hash the active sets to pick a
 * pipeline variant and splice the corresponding snippets into the
 * generated shader.
 *
 * The registry also caches per-frame material GPU-data blocks: every
 * shape that references the same IProgram reuses a single
 * `write_draw_data` upload, and the BVH builder / RT primary walker
 * can both ask for the same prog's `(mat_id, mat_addr)` without
 * duplicating work.
 *
 * Lifetime: the persistent maps live across frames so compiled
 * pipelines stay cached. `begin_frame` resets only the per-frame
 * state.
 */
class FrameSnippetRegistry
{
public:
    struct MaterialInfo
    {
        string_view fn_name;      ///< velk_fill_<name>() (pointer into material-owned source).
        string      include_name; ///< "<fn_name>.glsl" owned copy for render-context includes.
    };

    struct ShadowTechInfo
    {
        string_view fn_name;
        string      include_name;
    };

    struct IntersectInfo
    {
        string_view fn_name;
        string      include_name;
    };

    /// @brief Clears per-frame state; persistent maps stay.
    void begin_frame();

    /// @brief Registers a material's snippet on first sight; returns
    ///        its stable id. Non-IShaderSnippet programs return 0.
    uint32_t register_material(IProgram* prog, IRenderContext& ctx);

    /// @brief Registers a shadow technique's snippet on first sight.
    uint32_t register_shadow_tech(IShadowTechnique* tech, IRenderContext& ctx);

    /// @brief Registers a visual's custom intersect snippet and
    ///        returns its dispatch id. Shapes emitted by the visual
    ///        stamp this id into `shape_kind` so the composed
    ///        `intersect_shape` function routes them to the snippet.
    ///        Returns 0 for visuals without a snippet.
    uint32_t register_intersect(IAnalyticShape* shape, IRenderContext& ctx);

    /// @brief Returns a material's `(mat_id, mat_addr)` for this frame.
    ///        Writes draw-data to the frame buffer once per unique
    ///        program; subsequent lookups reuse the cached address.
    struct MaterialRef
    {
        uint32_t mat_id = 0;
        uint64_t mat_addr = 0;
    };
    MaterialRef resolve_material(IProgram* prog, IRenderContext& ctx, FrameDataManager& fb);

    const vector<MaterialInfo>&   material_info_by_id() const { return material_info_by_id_; }
    const vector<ShadowTechInfo>& shadow_tech_info_by_id() const { return shadow_tech_info_by_id_; }
    const vector<IntersectInfo>&  intersect_info_by_id() const { return intersect_info_by_id_; }
    const vector<uint32_t>&       frame_materials() const { return frame_materials_; }
    const vector<uint32_t>&       frame_shadow_techs() const { return frame_shadow_techs_; }
    const vector<uint32_t>&       frame_intersects() const { return frame_intersects_; }

private:
    struct MaterialInstance
    {
        IProgram* prog = nullptr;
        uint32_t  mat_id = 0;
        uint64_t  mat_addr = 0;
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
};

} // namespace velk::ui

#endif // VELK_UI_FRAME_SNIPPET_REGISTRY_H
