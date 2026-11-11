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

/// The clusters chosen in a cull start with this many words: how many records follow, and the
/// group counts of the dispatch that sorts them. A record is an entry, a cluster, a slot and
/// an error. See `instance_cluster_fill_src`.
inline constexpr uint32_t kCullChosenHeaderWords = 4;
inline constexpr uint32_t kCullChosenRecordWords = 4;
/// Words in a cluster draw entry: see `instance_cluster_runs_src`.
inline constexpr uint32_t kClusterEntryWords = 14;
/// Where in a cluster draw entry the draw data record its pixels name is, written when the
/// draw is emitted, so that tiny clusters can name it too. 0xFFFFFFFF until then.
inline constexpr uint32_t kClusterEntryDrawWord = 13;

/**
 * @brief What GPU culling needs to know about a batch. Batches with the same @c key
 *        are filled by one dispatch. Word offsets are into the culling arena.
 */
struct InstanceCullInfo
{
    const void* key = nullptr;
    uint32_t instances_base = 0;  ///< Element base of the instances in the instance arena.
    uint32_t slot_count = 0;      ///< Instances to test.
    uint32_t table_word = 0;      ///< The dispatch's table: see `instance_cull_compute_src`.
    uint32_t visible_word = 0;    ///< The list of instance slots this batch's draw reads.
    uint32_t count_word = 0;      ///< How many visible slots its mesh has, once culled.
    /// Set when it is drawn by cluster: one indexed command per cluster in use, each instanced
    /// over a run of its list. See `instance_cluster_runs_src`.
    uint32_t cluster_count = 0;          ///< The most commands. 0 when it is not.
    uint32_t cluster_entry_word = 0;     ///< Its entry, for the passes that fill its commands.
    uint32_t cluster_commands_word = 0;  ///< Its commands in the culling arena.
    uint32_t cluster_draws_word = 0;     ///< How many commands are in use, in the culling arena.
    uint32_t cluster_vertex_word = 0xFFFFFFFFu;  ///< The hierarchy's own vertices in the mesh-word arena, if it has any.
    uint32_t cluster_index_word = 0;     ///< Its indices in the mesh-word arena.
    uint32_t cluster_errors_word = 0;    ///< Each list position's surface error in world units, in the culling arena.
    uint32_t cluster_capacity = 0;       ///< How many positions its list has.
    uint32_t cluster_second_commands_word = 0;  ///< The commands of the second occlusion phase.
    uint32_t cluster_second_draws_word = 0;     ///< How many of those are in use.
    uint32_t second_args_word = 0xFFFFFFFFu;    ///< A plain instanced draw's arguments for that phase, in the culling arena.
    uint32_t second_args_first_word = 0;        ///< Where in those its first instance is.
    uint32_t chosen_word = 0;            ///< The clusters chosen for its visual. Unused when culled together.
    uint32_t chosen_capacity = 0;        ///< How many records fit there.
    /// Culled in one dispatch with the other batches that set this, instead of on its own.
    bool cull_together = false;
    float max_distance = 0.f;     ///< Instances further from the camera are culled. 0 means no limit.
    bool far_form = false;                  ///< Whether instances too small to draw get averaged instead.
    bool has_unloaded = false;              ///< Whether partitions can be without their instances: see `instance_box_splat_src`.
    uint32_t medium_id = 0;                 ///< What those partitions' BVH leaves are marked with.
    float far_albedo[3] = {1.f, 1.f, 1.f};  ///< Colour of those instances.
    float far_facing[kSurfaceDirectionCount] = {};  ///< Their surface area by which way it faces.
    /// Partitions to test before instances: see `instance_partition_cull_src`. Without
    /// them, every slot is tested.
    uint32_t partition_table_word = 0;
    uint32_t partition_count = 0;
    uint32_t chunk_list_word = 0;
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
 * resolved through `IRenderContext::pipelines().find()`. `texture_key` is
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
    ///        `IRenderContext::pipelines().find()`. 0 if no pipeline yet.
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
    ///        consumers borrow the raw pointer. Null, or not yet resolvable
    ///        through the resource manager, until the blob is resident.
    virtual IBuffer* storage_buffer() const = 0;

    /// @}

    /// @brief Fills @p out and returns true if this batch's instances are culled on
    ///        the GPU. Its indirect args then get their instance count from the
    ///        culling pass.
    virtual bool get_instance_cull(InstanceCullInfo& out) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_BATCH_H
