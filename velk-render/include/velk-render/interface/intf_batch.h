#ifndef VELK_RENDER_INTF_BATCH_H
#define VELK_RENDER_INTF_BATCH_H

#include <velk/array_view.h>
#include <velk/api/math_types.h>
#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_state.h>
#include <velk-render/interface/intf_shader_source.h>

#include <cstdint>

namespace velk {

class ArenaRegion;

/**
 * @brief Persistent per-batch storage layout shared between producers
 *        (BatchBuilder fills the prefix), the IBatch implementation
 *        (`get_data` returns the contiguous blob), and consumers
 *        (`emit_draw_calls` reads several offsets into the same buffer).
 *
 * Layout: `[args (32 B)][count (16 B)]` = 48 B fixed.
 *
 * - args  (offset 0, 32 B) — indirect-draw command record. 16-byte
 *   aligned and oversized so any future args struct fits.
 * - count (offset 32, 16 B) — uint32 actual draw count consumed by
 *   the backend's indirect-with-count draw; 16-byte aligned.
 *
 * That is the whole of it: what remains here is only what the GPU reads as
 * a buffer in its own right. Everything the shader reads has moved to a
 * shared arena and is reached by index — instances (set = 1 slot 3, see
 * `set_instance_region`), materials (slot 4, the material's own region),
 * and the `DrawDataHeader` itself (slot 15, see `set_draw_data_region`).
 *
 * The DrawDataHeader is not here either: it lives in the shared draw-data
 * arena (set = 1 slot 15), and the DrawCall's root_constants carry its
 * element base. What remains is only what Vulkan itself must read as a
 * buffer: the indirect args and the draw count.
 */
struct BatchBufferLayout
{
    static constexpr size_t kArgsOffset        = 0;
    static constexpr size_t kArgsSize          = 32;
    static constexpr size_t kCountOffset       = kArgsOffset + kArgsSize;
    static constexpr size_t kCountSize         = 16;
    static constexpr size_t kBufferSize        = kCountOffset + kCountSize; // 48
};

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
 * resolved through `IRenderContext::find_pipeline()`. `texture_key` is
 * the bindless-source ISurface address resolved at emit time.
 * `instance_data` carries per-instance bytes the vertex shader reads by
 * index from the shared instance arena. `world_aabb` is the union of
 * every contained instance's bounds, used by frustum culling at emit
 * time.
 */
class IBatch
    : public Interface<IBatch, IRenderState,
                       VELK_UID("a8a39f1c-b3e5-4e5a-9d0e-c9c2dad6a2ef")>
{
public:
    /// @brief Stable hash on visual class / material; resolved through
    ///        `IRenderContext::find_pipeline()`. 0 if no pipeline yet.
    virtual uint64_t pipeline_key() const = 0;

    /// @brief Bindless-source ISurface address, or 0 when unused.
    virtual uint64_t texture_key() const = 0;

    /// @brief Per-instance bulk bytes the vertex shader reads via a
    ///        buffer-reference dereference.
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
    ///        instance_index * instance_stride + bytes.size())` within
    ///        the batch's suballocated region in the shared instance
    ///        arena, written in place through the pointer captured by
    ///        `set_instance_binding`. @p bytes.size() must be
    ///        `<= instance_stride`. Out-of-range slots are silently ignored.
    virtual void update_instance_at(uint32_t instance_index,
                                    array_view<const uint8_t> bytes) = 0;

    /// @name Shared instance arena binding — instance bytes live in the
    ///       Renderer-owned instance arena (set = 1 slot 3), not in the
    ///       per-batch storage buffer. The batch owns a persistent
    ///       `ArenaRegion` (allocated on structural change, kept across
    ///       steady-state frames, RAII-freed when the batch is destroyed);
    ///       the vertex shader reads `velk_instances.data[instances_base + i]`
    ///       where `instances_base = offset / instance_stride`.
    /// @{
    /// @brief Takes ownership of this batch's instance region. Moving in a new
    ///        region RAII-frees the previous one (deferred past the in-flight
    ///        fence by the arena). Pass a default (empty) region to release.
    virtual void set_instance_region(ArenaRegion&& region) = 0;

    /// @brief Byte offset of this batch's instance region within the arena.
    ///        `offset / instance_stride` is the shader `instances_base`.
    virtual uint64_t instance_region_offset() const = 0;

    /// @brief Byte size of this batch's instance region (0 if none).
    virtual uint64_t instance_region_size() const = 0;

    /// @brief Returns whether the instance bytes changed since the last call
    ///        and clears the flag. Set by `finalize_storage` (structural
    ///        rebuild) and `update_instance_at`; the upload sweep re-uploads
    ///        the region only when set, so unchanged batches never re-upload.
    virtual bool take_instances_dirty() = 0;
    /// @}

    /// @name Per-batch draw-data region — this batch's DrawDataHeader record
    ///       in the shared draw-data arena (set = 1 slot 15). Persistent, so
    ///       the element base baked into a recorded draw stays valid; every
    ///       batch has one, including those with no storage buffer.
    /// @{
    /// @brief Takes ownership of this batch's draw-data region. Moving in a
    ///        new region RAII-frees the previous one (deferred past the
    ///        in-flight fence). Pass a default (empty) region to release.
    virtual void set_draw_data_region(ArenaRegion&& region) = 0;

    /// @brief Byte offset of this batch's draw-data region within the arena.
    ///        `offset / sizeof(DrawDataHeader)` is the shader's `draw_base`.
    virtual uint64_t draw_data_region_offset() const = 0;

    /// @brief Byte size of this batch's draw-data region (0 if none).
    virtual uint64_t draw_data_region_size() const = 0;
    /// @}

    /// @name Persistent per-batch storage — each batch composes an
    ///       `IBuffer` (an `impl::GpuBuffer` instance) holding the
    ///       `BatchBufferLayout` blob, allocated and uploaded by the
    ///       renderer's standard buffer pipeline
    ///       (`IGpuResourceManager::ensure_buffer_storage`).
    ///       `emit_draw_calls` resolves the backend handle via
    ///       `IGpuResourceManager::find_buffer(storage_buffer())->handle`
    ///       for indirect args + count. Instance bytes and the draw header
    ///       live in shared arenas, not here.
    /// @{
    /// @brief Composed storage buffer. Lifetime is owned by the batch;
    ///        consumers borrow the raw pointer.
    virtual IBuffer* storage_buffer() const = 0;

    /// @brief GPU virtual address of the start of the storage blob.
    virtual uint64_t storage_gpu_address() const = 0;

    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_BATCH_H
