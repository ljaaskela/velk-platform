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
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/material/intf_material_internal.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/render_types.h>

namespace velk {

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
 * @param default_uv1 Fallback UV1 buffer (read as index 0). Pass the
 *        renderer's context-owned default; null skips batches without
 *        their own UV1 stream.
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
    IBuffer* default_uv1,
    uint32_t view_globals_base,
    ResolvePipelineFn resolve_pipeline,
    const ::velk::render::Frustum* frustum = nullptr)
{
    VELK_PERF_SCOPE("renderer.emit_draw_calls");

    for (auto& batch_ptr : batches) {
        if (!batch_ptr) continue;
        const IBatch& batch = *batch_ptr;

        if (frustum && !::velk::render::aabb_in_frustum(*frustum, batch.world_aabb())) {
            continue;
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
        const bool has_storage = (storage_gb != nullptr && batch.storage_gpu_address() != 0);
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

        auto primitive_ptr = batch.primitive();
        IMeshPrimitive* primitive = primitive_ptr.get();
        if (!primitive) continue;
        auto buffer = primitive->get_buffer();
        if (!buffer) continue;

        // IBO half is optional: indexed draw when ibo_size > 0, plain
        // vkCmdDraw when 0 (e.g. TriangleStrip unit quad). The indices live in
        // the shared mesh-word arena, so the bound buffer is the arena's
        // backing buffer and the offset is this mesh's region plus the IBO
        // half's position within it. The arena handle is re-asked rather than
        // cached because growth replaces the backing buffer.
        IGpuBuffer* ibo_gb = nullptr;
        size_t ibo_offset = 0;
        if (buffer->get_ibo_size() > 0) {
            const GpuRef geometry = get_gpu_ref(buffer);
            if (geometry.kind != GpuRef::Kind::Index) continue;
            auto arena = resources.shared_arena(geometry.get_slot(), 4);
            if (!arena) continue;
            ibo_gb = arena->buffer();
            if (!ibo_gb) continue;
            ibo_offset = size_t(geometry.get_base()) * 4u + buffer->get_ibo_offset();
        }

        DrawDataHeader header{};
        header.globals_base = view_globals_base;
        header.instances_base = instances_base;
        header.texture_id = texture_id;
        header.instance_count = batch.instance_count();
        // Vertex streams are word bases into the mesh-word arena, the same
        // bytes and the same model RT reads.
        const GpuRef vbo_ref = get_gpu_ref(buffer);
        if (vbo_ref.kind != GpuRef::Kind::Index) continue;
        header.vbo_base = vbo_ref.get_base();

        if (auto uv1 = primitive->get_uv1_buffer()) {
            const GpuRef uv1_ref = get_gpu_ref(uv1);
            if (uv1_ref.kind != GpuRef::Kind::Index) continue;
            header.uv1_base = uv1_ref.get_base() + primitive->get_uv1_offset() / 4u;
            header.uv1_enabled = 1;
        } else {
            const GpuRef uv1_ref = get_gpu_ref(default_uv1);
            if (uv1_ref.kind != GpuRef::Kind::Index) continue;
            header.uv1_base = uv1_ref.get_base();
            header.uv1_enabled = 0;
        }

        // Material draw-data lives in the shared material arena (set = 1 slot
        // 4), suballocated + written by ViewPreparer's upload sweep. The header
        // carries the element base (region offset / record size) the fragment
        // shader adds to reach velk_materials.data[material_base]. Materials
        // with no draw data (record size 0) get base 0.
        auto material_ptr = batch.material();
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
        if (batch.draw_data_region_size() != sizeof(DrawDataHeader)) continue;
        auto draw_arena = resources.shared_arena(IRenderBackend::kGlobalDrawData,
                                                 sizeof(DrawDataHeader));
        if (!draw_arena) continue;
        const uint64_t draw_data_offset = batch.draw_data_region_offset();
        draw_arena->write_at(draw_data_offset, &header, sizeof(header));
        const uint32_t draw_base =
            static_cast<uint32_t>(draw_data_offset / sizeof(DrawDataHeader));

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

        call.root_constants_size = sizeof(uint32_t);
        std::memcpy(call.root_constants, &draw_base, sizeof(uint32_t));

        out_calls.push_back(call);
    }
}

} // namespace velk

#endif // VELK_RENDER_FRAME_DRAW_CALL_EMIT_H
