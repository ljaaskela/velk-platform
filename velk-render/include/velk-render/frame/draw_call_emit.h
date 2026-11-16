#ifndef VELK_RENDER_FRAME_DRAW_CALL_EMIT_H
#define VELK_RENDER_FRAME_DRAW_CALL_EMIT_H

#include "velk-render/gpu_data.h"

#include <velk/api/perf.h>

#include <cstdlib>
#include <cstring>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/interface/intf_frame_snippet_registry.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/material/intf_material_internal.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/render_types.h>

namespace velk {

/// The context-owned buffers a draw record points at for the optional vertex streams a
/// primitive does not have (DefaultBufferType). See default_streams.
struct DefaultStreams
{
    IBuffer* uv1 = nullptr;
    IBuffer* color = nullptr;
};

/// What a view's culling leaves for draws that their own batch's culling does not list: the
/// view's quads, and the batches drawn by their own pass that the plain objects' kept cut holds
/// for rays. See instance_cut_draw_words.
struct CutDrawWords
{
    uint32_t quads_word = kNoVisibleList;  ///< The view's quad list (kQuadListHeaderWords).
    uint32_t quad_capacity = 0;            ///< Quads it holds, which its column of records follows.
    /// For each batch drawn by its own pass, the word of the kept cut record of its first
    /// instance, followed by its other instances'. A word is 0 for an instance from no record.
    vector<std::pair<const IBatch*, uint32_t>> own_records;
};

/// What write_draw_data found of a batch that a draw of it needs.
struct WrittenDrawData
{
    IMeshPrimitive::Ptr primitive;
    IGpuBuffer* index_buffer = nullptr;  ///< Null for a draw without indices.
    size_t index_offset = 0;
    InstanceCullInfo cull_info{};
    uint32_t draw_base = 0;  ///< The record's index, which the draw and its pixels name.
};

/**
 * @brief Writes @p batch's draw record (DrawDataHeader) in its region of the shared draw data
 *        arena: where its instances, vertices, indices, texture and material are. What a pixel
 *        or a ray that names the draw reads (VelkDrawData). Also tells a batch drawn by cluster
 *        which record its clusters name (kClusterEntryDrawWord). Returns false for a batch with
 *        nothing to draw yet.
 */
inline bool write_draw_data(const IBatch& batch, IGpuResourceManager& resources, const DefaultStreams& defaults,
                            uint32_t view_globals_base, IFrameSnippetRegistry* snippets,
                            const FrameResolveContext* resolve_ctx, WrittenDrawData& out,
                            const CutDrawWords* cut_words = nullptr)
{
    const uint32_t quads_word = cut_words ? cut_words->quads_word : kNoVisibleList;
    // Instance data lives in the shared instance arena (set = 1 slot 3),
    // suballocated per batch by the upload sweep. The header carries the
    // element base (region offset / stride) the vertex shader adds to
    // gl_InstanceIndex. Batches with no instance data (e.g. env) get 0.
    uint32_t instances_base = 0;
    if (batch.instance_stride() != 0 && batch.instance_region_size() != 0) {
        instances_base = static_cast<uint32_t>(
            batch.instance_region_offset() / batch.instance_stride());
    }
    uint32_t texture_id = 0;
    if (batch.texture_key() != 0) {
        auto* tex = reinterpret_cast<ISurface*>(batch.texture_key());
        if (auto* gt = resources.find_texture(tex)) {
            texture_id = get_texture_id(gt);
        }
        if (texture_id == 0) {
            uint64_t rt_id = get_render_target_id(tex);
            if (rt_id != 0) texture_id = static_cast<uint32_t>(rt_id);
        }
    }

    out.primitive = batch.primitive();
    IMeshPrimitive* primitive = out.primitive.get();
    if (!primitive) return false;
    auto buffer = primitive->get_buffer();
    if (!buffer) return false;

    // IBO half is optional: indexed draw when ibo_size > 0, plain
    // vkCmdDraw when 0 (e.g. TriangleStrip unit quad). The indices live in
    // the shared mesh-word arena, so the bound buffer is the arena's
    // backing buffer and the offset is this mesh's region plus the IBO
    // half's position within it. The arena handle is re-asked rather than
    // cached because growth replaces the backing buffer.
    uint32_t index_base = kNoVisibleList;
    if (buffer->get_ibo_size() > 0) {
        const GpuRef geometry = get_gpu_ref(buffer);
        if (geometry.kind != GpuRef::Kind::Index) return false;
        auto arena = resources.shared_arena(geometry.get_slot(), 4);
        if (!arena) return false;
        out.index_buffer = arena->buffer();
        if (!out.index_buffer) return false;
        out.index_offset = size_t(geometry.get_base()) * 4u + buffer->get_ibo_offset();
        index_base = static_cast<uint32_t>(out.index_offset / 4u);
    }

    DrawDataHeader header{};
    header.globals_base = view_globals_base;
    header.instances_base = instances_base;
    header.texture_id = texture_id;
    InstanceCullInfo& cull_info = out.cull_info;
    const bool culled = batch.get_instance_cull(cull_info);
    header.visible_base = culled ? cull_info.visible_word : kNoVisibleList;
    header.record_base = culled ? cull_info.records_word : kNoVisibleList;
    // The view's quads: their list is the visible list, each an instance of the unit quad.
    if (batch.draws_quads()) {
        if (quads_word == kNoVisibleList) return false;
        header.flags |= kDrawFlagQuads;
        header.visible_base = quads_word + kQuadListHeaderWords;
        header.record_base = header.visible_base + cut_words->quad_capacity;
    } else if (!culled && cut_words) {
        for (const auto& [own, word] : cut_words->own_records) {
            if (own == &batch) {
                header.record_base = word;
            }
        }
    }
    // A draw by cluster numbers triangles within its cluster, of up to 256.
    const uint32_t triangles = cull_info.cluster_count != 0 ? 256u
        : primitive->get_index_count() != 0                 ? primitive->get_index_count() / 3u
                                                            : primitive->get_vertex_count();
    while (header.triangle_bits < 31u && (uint64_t(1) << header.triangle_bits) < triangles) {
        ++header.triangle_bits;
    }
    // A cluster's record says where its indices are in the arena, and its vertices and
    // second uvs, which the ones below stand in for only when it is not drawn by cluster.
    header.index_base = cull_info.cluster_count != 0 ? 0u : index_base;
    header.cluster_base = cull_info.cluster_count != 0
        ? cull_info.records_word + cull_info.cluster_capacity
        : kNoVisibleList;
    // Vertex streams are word bases into the mesh-word arena, the same
    // bytes and the same model RT reads.
    const GpuRef vbo_ref = get_gpu_ref(buffer);
    if (vbo_ref.kind != GpuRef::Kind::Index) return false;
    header.vbo_base = vbo_ref.get_base();

    if (auto uv1 = primitive->get_uv1_buffer()) {
        const GpuRef uv1_ref = get_gpu_ref(uv1);
        if (uv1_ref.kind != GpuRef::Kind::Index) return false;
        header.uv1_base = uv1_ref.get_base() + primitive->get_uv1_offset() / 4u;
        header.flags |= kDrawFlagUv1;
    } else {
        const GpuRef uv1_ref = get_gpu_ref(defaults.uv1);
        if (uv1_ref.kind != GpuRef::Kind::Index) return false;
        header.uv1_base = uv1_ref.get_base();
    }
    // A colour for each vertex of the vertex buffer, indexed as the indices index it.
    const auto color = primitive->get_color_buffer();
    const GpuRef color_ref = get_gpu_ref(color ? interface_cast<IBuffer>(color.get()) : defaults.color);
    if (color_ref.kind != GpuRef::Kind::Index) return false;
    header.color_base = color_ref.get_base();
    header.flags |= color ? kDrawFlagColor : 0u;

    // Material draw-data lives in the shared material arena (set = 1 slot
    // 4), suballocated + written by ViewPreparer's upload sweep. The header
    // carries the element base (region offset / record size) the fragment
    // shader adds to reach velk_materials.data[material_base]. Materials
    // with no draw data (record size 0) get base 0.
    auto material_ptr = batch.material();
    if (snippets && resolve_ctx && material_ptr) {
        const auto ref = snippets->resolve_material(material_ptr.get(), *resolve_ctx);
        header.material_id = ref.mat_id;
        header.material_words = ref.mat_base;
    }
    if (auto* mi = interface_cast<IMaterialInternal>(material_ptr.get())) {
        if (auto* dd = interface_cast<IDrawData>(material_ptr.get())) {
            const size_t rec = dd->get_draw_data_size();
            auto buf = mi->material_buffer();
            if (rec != 0 && buf) {
                header.material_base =
                    static_cast<uint32_t>(get_gpu_ref(buf).get_base() / rec);
            }
        }
    }

    // The header goes to this batch's persistent region of the shared
    // draw-data arena, allocated by the upload sweep. Persistent rather
    // than per-frame because the base below is baked into the recorded
    // draw call: a rotating region would go stale on any frame the command
    // buffer is reused rather than re-recorded.
    if (batch.draw_data_region_size() != sizeof(DrawDataHeader)) return false;
    auto draw_arena = resources.shared_arena(IRenderBackend::kGlobalDrawData,
                                             sizeof(DrawDataHeader));
    if (!draw_arena) return false;
    const uint64_t draw_data_offset = batch.draw_data_region_offset();
    draw_arena->update_at(draw_data_offset, &header, sizeof(header));
    const uint32_t draw_base =
        static_cast<uint32_t>(draw_data_offset / sizeof(DrawDataHeader));
    out.draw_base = draw_base;
    if (cull_info.cluster_count != 0) {
        if (auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t))) {
            culling->update_at(uint64_t(cull_info.cluster_entry_word + kClusterEntryDrawWord) * sizeof(uint32_t),
                               &draw_base, sizeof(draw_base));
        }
    }
    // Likewise the quad draw, beside its list (kQuadListHeaderWords), for rays that hit a quad.
    if (batch.draws_quads()) {
        if (auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t))) {
            culling->update_at(uint64_t(quads_word + 5u) * sizeof(uint32_t), &draw_base, sizeof(draw_base));
        }
    }
    return true;
}

/**
 * @brief Writes the draw record of each of @p batches (write_draw_data), for a path that
 *        draws none of them but traces rays, which name draws. Each frame, as a path that
 *        draws them writes them.
 */
inline void write_draw_data(const vector<IBatch::Ptr>& batches, IGpuResourceManager& resources,
                            const DefaultStreams& defaults, uint32_t view_globals_base, IFrameSnippetRegistry* snippets,
                            const FrameResolveContext* resolve_ctx)
{
    for (const auto& batch : batches) {
        WrittenDrawData written;
        if (batch) {
            write_draw_data(*batch, resources, defaults, view_globals_base, snippets, resolve_ctx, written);
        }
    }
}

/**
 * @brief Iterates batches and emits a `DrawCall` per batch.
 *
 * Generic per-batch glue: frustum cull, instance / material data
 * upload, IBO + UV1 stream resolution, texture id resolution. Per-
 * batch pipeline resolution is delegated to @p resolve_pipeline so
 * each render path picks its own composition strategy (forward fragment
 * vs. deferred gbuffer fragment etc.) without baking that knowledge
 * into the renderer or `IRenderContext`.
 *
 * The resolver runs once per surviving batch and returns the backend
 * `PipelineId` to bind. Returning 0 skips the batch.
 *
 * @param defaults The context-owned buffers for vertex streams a primitive
 *        does not have (default_streams); a batch without its own UV1 or
 *        colours is skipped when they are null.
 *
 * Template parameter inlines the resolver call per call site so
 * captureful lambdas don't pay a virtual-dispatch tax in the hot loop.
 *
 * Draw calls are appended to @p out_calls — callers pass one vector
 * per pass and move it into the resulting `Submit` op. Reusing a
 * caller-owned vector across multiple `emit_draw_calls` invocations
 * (e.g. ForwardPath's env + main batches in one pass) saves the
 * per-call allocation.
 */
template <typename ResolvePipelineFn>
inline void emit_draw_calls(
    vector<DrawCall>& out_calls,
    const vector<IBatch::Ptr>& batches,
    IFrameDataManager& frame_data,
    IGpuResourceManager& resources,
    const DefaultStreams& defaults,
    uint32_t view_globals_base,
    ResolvePipelineFn resolve_pipeline,
    const ::velk::render::Frustum* frustum = nullptr,
    IFrameSnippetRegistry* snippets = nullptr,
    const FrameResolveContext* resolve_ctx = nullptr,
    bool second_phase = false,
    const CutDrawWords* cut_words = nullptr)
{
    VELK_PERF_SCOPE("renderer.emit_draw_calls");
    const uint32_t quads_word = cut_words ? cut_words->quads_word : kNoVisibleList;

    for (auto& batch_ptr : batches) {
        if (!batch_ptr) continue;
        const IBatch& batch = *batch_ptr;

        if (frustum && !::velk::render::aabb_in_frustum(*frustum, batch.world_aabb())) {
            continue;
        }

        // The second occlusion phase only draws what GPU culling can have held back.
        if (second_phase) {
            InstanceCullInfo info{};
            if (!batch.get_instance_cull(info) || (info.cluster_count == 0 && info.second_args_word == 0xFFFFFFFFu)) {
                continue;
            }
        }

        IGpuPipeline* pipeline = resolve_pipeline(batch);
        if (!pipeline) continue;

        // Each batch composes an IBuffer (impl::GpuBuffer) holding the
        // [args(32)][count(16)] blob, which is all that still has to be a
        // buffer the GPU reads directly: the indirect commands. Instance,
        // material and header bytes all live in shared arenas, reached by
        // index. Batches with no backing buffer yet (env_batch, outside the
        // upload pipeline) fall through to per-frame staging for the
        // indirect commands only.
        IBuffer* storage_buf = batch.storage_buffer();
        IGpuBuffer* storage_gb = nullptr;
        if (storage_buf) {
            if (auto* be = resources.find_buffer(storage_buf)) {
                if (auto sgb = be->buffer.lock()) {
                    storage_gb = sgb.get();
                }
            }
        }
        // A resolved, still-live IGpuBuffer is exactly the residency question:
        // the manager only registers an entry once the allocation succeeded.
        const bool has_storage = (storage_gb != nullptr);
        WrittenDrawData written;
        if (!write_draw_data(batch, resources, defaults, view_globals_base, snippets, resolve_ctx, written, cut_words)) {
            continue;
        }
        IMeshPrimitive* primitive = written.primitive.get();
        IGpuBuffer* ibo_gb = written.index_buffer;
        const size_t ibo_offset = written.index_offset;
        const InstanceCullInfo& cull_info = written.cull_info;

        // Always-indirect: pull args + count from the batch's own
        // storage buffer when available; fall back to writing a record
        // + uint32 count=1 into per-frame staging otherwise.
        DrawCall call{};
        call.pipeline = pipeline;
        call.indexed = (ibo_gb != nullptr);
        if (call.indexed) {
            call.index_buffer = ibo_gb;
            call.index_buffer_offset = ibo_offset;
            call.args_stride = sizeof(uint32_t) * 5; // VkDrawIndexedIndirectCommand
        } else {
            call.args_stride = sizeof(uint32_t) * 4; // VkDrawIndirectCommand
        }

        if (has_storage) {
            call.args_buffer        = storage_gb;
            call.args_buffer_offset = BatchBufferLayout::kArgsOffset;
            call.count_buffer        = storage_gb;
            call.count_buffer_offset = BatchBufferLayout::kCountOffset;
        } else {
            // Frame-staging fallback (env_batch et al.).
            if (call.indexed) {
                struct {
                    uint32_t indexCount;
                    uint32_t instanceCount;
                    uint32_t firstIndex;
                    int32_t  vertexOffset;
                    uint32_t firstInstance;
                } args{ primitive->get_index_count(), batch.instance_count(),
                        0, 0, 0 };
                uint64_t args_offset = frame_data.write(&args, sizeof(args));
                if (args_offset == IFrameDataManager::kInvalidOffset) continue;
                call.args_buffer = frame_data.active_buffer();
                call.args_buffer_offset = args_offset;
            } else {
                struct {
                    uint32_t vertexCount;
                    uint32_t instanceCount;
                    uint32_t firstVertex;
                    uint32_t firstInstance;
                } args{ primitive->get_vertex_count(), batch.instance_count(),
                        0, 0 };
                uint64_t args_offset = frame_data.write(&args, sizeof(args));
                if (args_offset == IFrameDataManager::kInvalidOffset) continue;
                call.args_buffer = frame_data.active_buffer();
                call.args_buffer_offset = args_offset;
            }
            uint32_t count_value = 1;
            uint64_t count_offset = frame_data.write(&count_value, sizeof(count_value), 4);
            if (count_offset == IFrameDataManager::kInvalidOffset) continue;
            call.count_buffer = frame_data.active_buffer();
            call.count_buffer_offset = count_offset;
        }
        call.max_draw_count = 1;

        // Drawn by cluster: its commands and their count come from the culling pass.
        if (cull_info.cluster_count != 0) {
            auto mesh_words = resources.shared_arena(IRenderBackend::kGlobalMeshWords, sizeof(uint32_t));
            auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
            IGpuBuffer* index_buffer = mesh_words ? mesh_words->buffer() : nullptr;
            IGpuBuffer* commands = culling ? culling->buffer() : nullptr;
            if (!index_buffer || !commands) continue;
            call.indexed = true;
            call.index_buffer = index_buffer;
            // A command's first index is its cluster's, counted from the start of the arena.
            call.index_buffer_offset = 0;
            call.args_stride = sizeof(uint32_t) * 5; // VkDrawIndexedIndirectCommand
            call.args_buffer = commands;
            call.args_buffer_offset =
                size_t(second_phase ? cull_info.cluster_second_commands_word : cull_info.cluster_commands_word) * sizeof(uint32_t);
            call.count_buffer = commands;
            call.count_buffer_offset =
                size_t(second_phase ? cull_info.cluster_second_draws_word : cull_info.cluster_draws_word) * sizeof(uint32_t);
            call.max_draw_count = cull_info.cluster_count;
        } else if (second_phase) {
            auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
            IGpuBuffer* args = culling ? culling->buffer() : nullptr;
            if (!args) continue;
            call.args_buffer = args;
            call.args_buffer_offset = size_t(cull_info.second_args_word) * sizeof(uint32_t);
        } else if (batch.draws_quads()) {
            // The view's quad list holds the draw's arguments and count (kQuadListHeaderWords).
            auto culling = resources.shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
            IGpuBuffer* args = culling ? culling->buffer() : nullptr;
            if (!args) continue;
            call.indexed = false;
            call.args_stride = sizeof(uint32_t) * 4;  // VkDrawIndirectCommand
            call.args_buffer = args;
            call.args_buffer_offset = size_t(quads_word) * sizeof(uint32_t);
            call.count_buffer = args;
            call.count_buffer_offset = size_t(quads_word + 4u) * sizeof(uint32_t);
            call.max_draw_count = 1;
        }

        call.root_constants_size = sizeof(uint32_t);
        std::memcpy(call.root_constants, &written.draw_base, sizeof(uint32_t));

        out_calls.push_back(call);
    }
}

} // namespace velk

#endif // VELK_RENDER_FRAME_DRAW_CALL_EMIT_H
