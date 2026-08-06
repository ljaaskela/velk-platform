#ifndef VELK_RENDER_INTF_FRAME_SNIPPET_REGISTRY_H
#define VELK_RENDER_INTF_FRAME_SNIPPET_REGISTRY_H

#include <velk/interface/intf_interface.h>
#include <velk/string.h>
#include <velk/string_view.h>
#include <velk/uid.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_buffer.h>

namespace velk {

class IProgram;
class IShadowTechnique;
class IAnalyticShape;
class IRenderContext;
class IFrameDataManager;
class IGpuResourceManager;
class IGpuArena;
struct FrameContext;

/**
 * @brief Bundle of velk-render-side per-frame deps a snippet registry
 *        needs to register snippets, upload material data buffers, and
 *        defer-destroy stale GPU handles.
 *
 * @c completion_marker is the GPU completion marker associated with the
 * in-flight frame; deferred-destroy entries tagged with it will be freed
 * once that frame's GPU work has finished. Obtain via
 * `IRenderBackend::pending_frame_completion_marker()`.
 */
struct FrameResolveContext
{
    IRenderContext*       render_ctx        = nullptr;
    IGpuResourceManager*  resources         = nullptr;
    IFrameDataManager*    frame_buffer      = nullptr;
    uint64_t              completion_marker = 0;
    /// Shared material arena (set = 1 slot 4). Materials read by index need
    /// their region to exist by the time shapes are stamped with its base,
    /// which happens during the BVH build, ahead of the renderer's material
    /// upload sweep; resolve_material allocates it here if it does not exist
    /// yet, the same way it ensures the persistent data buffer for materials
    /// still read by address.
    IGpuArena*            material_arena    = nullptr;
};

/**
 * @brief Scene-wide registry of composed shader snippets.
 *
 * Tracks stable small-integer ids for each material, shadow
 * technique, and analytic-shape intersect snippet the renderer has
 * seen, plus which of those ids are "active" in the current frame.
 * Each contributor implements `IShaderSource` and supplies its body
 * under a known role (`kEval`, `kShadow`, `kIntersect`); the registry
 * looks the source up at registration time. Composer-based pipelines
 * (RT compute, deferred lighting) hash the active sets to pick a
 * pipeline variant and splice the corresponding snippets in.
 *
 * The registry also caches per-frame material GPU-data blocks so a
 * material shared by N shapes only pays one upload.
 */
class IFrameSnippetRegistry
    : public Interface<IFrameSnippetRegistry, IInterface,
                       VELK_UID("6a013996-cdfa-4abf-a738-52eaf18d068f")>
{
public:
    struct MaterialInfo
    {
        string_view fn_name;      ///< `velk_eval_<X>` function name returning MaterialEval.
        string      include_name; ///< Owned include filename registered with IRenderContext.
        /// True when the compute-side include reads this material's record by
        /// index out of the shared material arena (via a generated unpacker)
        /// instead of chasing a device address. Producers filling RtShape use
        /// it to decide whether to stamp a word base or an address.
        bool        indexed = false;
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

    struct MaterialRef
    {
        uint32_t mat_id = 0;
        /// Word index of the material's record in the set = 1 material arena;
        /// goes onto the shape as RtShape::material_base.
        uint32_t mat_base = 0;
    };

    /// @brief Clears per-frame state; persistent maps stay.
    virtual void begin_frame() = 0;

    /// @brief Registers a material's eval snippet on first sight;
    ///        returns its stable id. Programs that don't implement
    ///        `IShaderSource` (or whose `kEval` role is empty) return 0.
    virtual uint32_t register_material(IProgram* prog, IRenderContext& ctx) = 0;

    /// @brief Registers a shadow technique's snippet on first sight.
    virtual uint32_t register_shadow_tech(IShadowTechnique* tech, IRenderContext& ctx) = 0;

    /// @brief Registers a visual's custom intersect snippet and returns
    ///        its dispatch id. Shapes emitted by the visual stamp this
    ///        id into `shape_kind`. Returns 0 for visuals without a snippet.
    virtual uint32_t register_intersect(IAnalyticShape* shape, IRenderContext& ctx) = 0;

    /// @brief Returns a material's `(mat_id, mat_base)` for this frame.
    ///        Writes draw-data to the frame buffer once per unique
    ///        program; subsequent lookups reuse the cached address.
    virtual MaterialRef resolve_material(IProgram* prog,
                                         const FrameResolveContext& ctx) = 0;

    virtual const vector<MaterialInfo>&   material_info_by_id() const = 0;
    virtual const vector<ShadowTechInfo>& shadow_tech_info_by_id() const = 0;
    virtual const vector<IntersectInfo>&  intersect_info_by_id() const = 0;
    virtual const vector<uint32_t>&       frame_materials() const = 0;
    virtual const vector<uint32_t>&       frame_shadow_techs() const = 0;
    virtual const vector<uint32_t>&       frame_intersects() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_FRAME_SNIPPET_REGISTRY_H
