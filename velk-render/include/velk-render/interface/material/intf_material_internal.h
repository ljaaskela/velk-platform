#ifndef VELK_RENDER_INTF_MATERIAL_INTERNAL_H
#define VELK_RENDER_INTF_MATERIAL_INTERNAL_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/types.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/material/intf_material.h>

namespace velk {

struct ShaderParam;

/**
 * @brief Internal interface for configuring a material after creation.
 *
 * Used by factory methods (e.g. IRenderContext::create_shader_material) to
 * inject reflected shader parameters and shader sources. Pipeline handle
 * storage is handled via IProgram::set_pipeline_handle on the program side.
 */
class IMaterialInternal : public Interface<IMaterialInternal, IMaterial>
{
public:
    /// Set up dynamic input properties from reflected shader parameters.
    /// Default implementation does nothing (e.g. ext::Material ignores this).
    virtual ReturnValue setup_inputs(const vector<ShaderParam>& /*params*/)
    {
        return ReturnValue::NothingToDo;
    }

    /// Provide the raw GLSL sources for materials that bypass the
    /// eval-driver (e.g. ShaderMaterial). The renderer will compile the
    /// pipeline lazily on first draw, reading the current IMaterialOptions
    /// attachment — so options set between creation and first draw are
    /// honored. Default implementation does nothing.
    virtual void set_sources(string_view /*vertex_source*/, string_view /*fragment_source*/) {}

    /// @name Shared material arena region — this material's draw data lives in
    ///       the Renderer-owned material arena (set = 1 slot 4). The upload
    ///       sweep allocates the region (aligned to the record size, so
    ///       `offset / record_size` is an integral base) and writes the bytes;
    ///       emit derives the shader `material_base` from the offset. The
    ///       region is RAII-freed (deferred past the in-flight fence) when the
    ///       material is destroyed. Default no-op for materials without
    ///       arena-backed data.
    /// @{
    virtual void set_material_region(ArenaRegion&& /*region*/) {}
    virtual uint64_t material_region_offset() const { return 0; }
    virtual uint64_t material_region_size() const { return 0; }

    /// Returns whether the material's draw-data bytes changed since the last
    /// call and clears the flag. The upload sweep rewrites the arena region
    /// only when set. A dedicated flag (not IBuffer::is_dirty) because the
    /// RT path consumes and clears the shared data buffer's dirty bit for its
    /// own upload; this keeps the arena write order-independent of that.
    /// Default true so materials without a tracked flag always re-upload.
    virtual bool take_material_dirty() { return true; }
    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_INTERNAL_H
