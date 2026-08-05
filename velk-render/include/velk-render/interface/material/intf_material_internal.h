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

    /// @name Shared material arena buffer — this material's draw data lives in
    ///       the Renderer-owned material arena (set = 1 slot 4), as an
    ///       arena-backed IBuffer the upload sweep writes `write_draw_data`
    ///       straight into. Its region is aligned to the record size, so both
    ///       consumers' divisions stay integral: raster reads
    ///       `offset / record_size`, the RT compute path `offset / 4`. The
    ///       region is freed (deferred past the in-flight fence) when the
    ///       material drops the buffer. Default no-op for materials without
    ///       arena-backed data.
    /// @{
    virtual void set_material_buffer(IBuffer::Ptr /*buf*/) {}
    virtual IBuffer::Ptr material_buffer() const { return {}; }

    /// Returns whether the material's record needs rewriting, and clears the
    /// flag. Driven by metadata change notifications, not by comparing bytes:
    /// arena memory is write-only, so there is nothing to compare against.
    /// Default true, so a material that tracks nothing is re-serialised every
    /// frame rather than going stale.
    ///
    /// @p texture_generation is IGpuResourceManager::texture_generation(). A
    /// record embeds TextureIds resolved when it was written, and nothing
    /// notifies when an id is assigned, so the material also reports dirty
    /// whenever that counter has moved since its last write. Both upload paths
    /// pass it, and whichever runs first for a given material consumes the
    /// change, so the record is rewritten once per generation rather than once
    /// per path.
    virtual bool take_material_dirty(uint64_t /*texture_generation*/) { return true; }

    /// Marks the record stale from a source the material cannot observe.
    /// State the material owns is covered automatically by metadata change
    /// notifications; a record derived from *external* state is not, and its
    /// owner must say so. TextMaterial is the worked example: its record holds
    /// bases into the font's glyph arenas, which move when the font bakes a
    /// glyph without anything writing the material itself.
    virtual void mark_material_dirty() {}
    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_INTERNAL_H
