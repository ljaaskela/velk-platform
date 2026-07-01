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

/**
 * @brief Persistent per-batch storage layout shared between producers
 *        (BatchBuilder fills the prefix), the IBatch implementation
 *        (`get_data` returns the contiguous blob), and consumers
 *        (`emit_draw_calls` reads several offsets into the same buffer).
 *
 * Layout: `[args (32 B)][count (16 B)][header (48 B)][material_ptr (8 B)][pad (8 B)]` = 112 B fixed.
 *
 * - args  (offset 0, 32 B) — indirect-draw command record. 16-byte
 *   aligned and oversized so any future args struct fits.
 * - count (offset 32, 16 B) — uint32 actual draw count consumed by
 *   the backend's indirect-with-count draw; 16-byte aligned.
 * - header (offset 48, 48 B) — `DrawDataHeader` the shader receives
 *   via push-constant. Persistent across frames; carries `instances_base`
 *   (index into the shared instance arena) and pointers to other
 *   persistent buffers (VBO, UV1), stable until the batch is rebuilt.
 * - material_ptr (offset 96, 8 B) — GPU pointer to the material's
 *   persistent `IProgramDataBuffer`. Stable per material lifetime.
 * - pad (offset 104, 8 B) — alignment to a 16-byte boundary.
 *
 * Instance bytes no longer live here: they are suballocated in the shared
 * persistent instance arena (set = 1 slot 3) and read by index; see
 * `set_instance_binding` / `instance_region_offset`.
 *
 * The DrawCall's root_constants carry `storage_gpu_address() + kHeaderOffset`
 * so the shader's push-constant pointer lands directly on the header.
 */
struct BatchBufferLayout
{
    static constexpr size_t kArgsOffset        = 0;
    static constexpr size_t kArgsSize          = 32;
    static constexpr size_t kCountOffset       = kArgsOffset + kArgsSize;
    static constexpr size_t kCountSize         = 16;
    static constexpr size_t kHeaderOffset      = kCountOffset + kCountSize;
    static constexpr size_t kHeaderSize        = 48;
    static constexpr size_t kMaterialPtrOffset = kHeaderOffset + kHeaderSize;
    static constexpr size_t kMaterialPtrSize   = 8;
    static constexpr size_t kBufferSize        = 112; // kMaterialPtrOffset + 16 (8 ptr + 8 pad)
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
 * `instance_data` carries per-instance bytes the vertex shader reads
 * via a buffer-reference dereference. `world_aabb` is the union of
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
    ///       Renderer-owned persistent instance arena (set = 1 slot 3), not
    ///       in the per-batch storage buffer. The upload sweep suballocates a
    ///       region, writes the bytes, and stamps the binding here; the
    ///       vertex shader reads `velk_instances.data[instances_base + i]`.
    /// @{
    /// @brief Stamped by the upload sweep after (re)allocating this batch's
    ///        instance region. @p arena owns the region (retained to free it
    ///        this frame; @p offset / @p size describe it (offset /
    ///        instance_stride = the shader `instances_base`).
    virtual void set_instance_binding(uint64_t offset, uint64_t size) = 0;

    /// @brief Byte offset of this batch's instance region within the arena's
    ///        current ring sub-buffer. `offset / instance_stride` is the
    ///        shader `instances_base`.
    virtual uint64_t instance_region_offset() const = 0;

    /// @brief Byte size written for this batch's instances this frame (0 if
    ///        the batch has no instance data).
    virtual uint64_t instance_region_size() const = 0;
    /// @}

    /// @name Persistent per-batch storage — each batch composes an
    ///       `IBuffer` (an `impl::GpuBuffer` instance) holding the
    ///       `BatchBufferLayout` blob, allocated and uploaded by the
    ///       renderer's standard buffer pipeline
    ///       (`IGpuResourceManager::ensure_buffer_storage`).
    ///       `emit_draw_calls` resolves the backend handle via
    ///       `IGpuResourceManager::find_buffer(storage_buffer())->handle`
    ///       for indirect args + count, and reads the header at
    ///       `storage_gpu_address() + kHeaderOffset`. Instance bytes live in
    ///       the shared instance arena, not here.
    /// @{
    /// @brief Composed storage buffer. Lifetime is owned by the batch;
    ///        consumers borrow the raw pointer.
    virtual IBuffer* storage_buffer() const = 0;

    /// @brief GPU virtual address of the start of the storage blob.
    virtual uint64_t storage_gpu_address() const = 0;

    /// @brief Host-visible mapped pointer to the storage blob, or
    ///        `nullptr` if the buffer hasn't been allocated yet (e.g.
    ///        env_batch with no persistent storage). Consumers use
    ///        `BatchBufferLayout` offsets to write per-batch data
    ///        (e.g. the `DrawDataHeader`) directly into the persistent
    ///        buffer; writes are visible to the GPU on the next submit
    ///        via host-coherent memory.
    virtual uint8_t* storage_mapped() const = 0;
    /// @}
};

} // namespace velk

#endif // VELK_RENDER_INTF_BATCH_H
