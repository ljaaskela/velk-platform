#ifndef VELK_RENDER_INTF_BATCH_H
#define VELK_RENDER_INTF_BATCH_H

#include <velk/array_view.h>
#include <velk/api/math_types.h>
#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_shader_source.h>

#include <cstdint>

namespace velk {

/**
 * @brief One draw-able primitive instance group.
 *
 * Built scene-side (today by `BatchBuilder` in velk-scene) and consumed
 * render-side by the path emitters and `emit_draw_calls`. Ptr-based so
 * the velk hive pools allocations and producers can cache per-frame
 * Ptr identity for future persistent-batch work.
 *
 * Fields are renderer-facing only — no scene types reach across this
 * boundary. `pipeline_key` is a stable hash on visual class / material;
 * resolved through `IRenderContext::pipeline_map()`. `texture_key` is
 * the bindless-source ISurface address resolved at emit time.
 * `instance_data` carries per-instance bytes the vertex shader reads
 * via a `buffer_reference`. `world_aabb` is the union of every contained
 * instance's bounds, used by frustum culling at emit time.
 */
class IBatch
    : public Interface<IBatch, IInterface,
                       VELK_UID("a8a39f1c-b3e5-4e5a-9d0e-c9c2dad6a2ef")>
{
public:
    /// @brief Stable hash on visual class / material; resolved through
    ///        `IRenderContext::pipeline_map()`. 0 if no pipeline yet.
    virtual uint64_t pipeline_key() const = 0;

    /// @brief Bindless-source ISurface address, or 0 when unused.
    virtual uint64_t texture_key() const = 0;

    /// @brief Per-instance bulk bytes the vertex shader reads via
    ///        buffer_reference.
    virtual array_view<const uint8_t> instance_data() const = 0;

    /// @brief Bytes per instance in `instance_data`.
    virtual uint32_t instance_stride() const = 0;

    /// @brief Number of instances. `instance_data.size() == instance_stride * instance_count`.
    virtual uint32_t instance_count() const = 0;

    /// @brief Union of every contained instance's world bounds. Used
    ///        by frustum culling at emit time.
    virtual aabb world_aabb() const = 0;

    /// @brief Material program. Null for batches whose pipeline is
    ///        fully driven by a material on the entry.
    virtual IProgram::Ptr material() const = 0;

    /// @brief Mesh primitive (vertex / index buffers).
    virtual IMeshPrimitive::Ptr primitive() const = 0;

    /// @brief GLSL source contributor for this batch's visual. Each
    ///        render path queries the roles it needs. Null for batches
    ///        whose pipeline is fully driven by a material on the entry.
    virtual IShaderSource::Ptr shader_source() const = 0;

    /// @brief Captured at batch-build time so build_draw_calls can
    ///        lazy-compile the pipeline against any target format on
    ///        cache miss without re-reading the visual / material storage.
    virtual PipelineOptions pipeline_options() const = 0;

    /// @brief Overwrite one instance's bytes in-place. Used by the
    ///        scene-side incremental update path to push a fresh world
    ///        matrix (or any transform-only payload) into an existing
    ///        slot without touching the rest of the batch. The byte
    ///        range overwritten is `[instance_index * instance_stride,
    ///        instance_index * instance_stride + bytes.size())`.
    ///        @p bytes.size() must be `<= instance_stride`. Out-of-range
    ///        slots are silently ignored.
    virtual void update_instance_at(uint32_t instance_index,
                                    array_view<const uint8_t> bytes) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_BATCH_H
