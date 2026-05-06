#ifndef VELK_RENDER_FRAME_DRAW_CALL_EMIT_H
#define VELK_RENDER_FRAME_DRAW_CALL_EMIT_H

#include "velk-render/gpu_data.h"

#include <velk/api/perf.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/frustum.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_texture_resolver.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief Per-frame cache used to dedupe material upload work across
 *        batches that share an IProgram.
 *
 * `IRenderContext::build_draw_calls` (and any future draw-call emitters)
 * write a material's draw-data to the frame buffer once per unique
 * program per frame and reuse the cached GPU address for subsequent
 * batches. Pass a fresh (or `clear()`-ed) cache each frame so addresses
 * don't leak across frames.
 */
struct MaterialAddrCache
{
    std::unordered_map<IProgram*, uint64_t> addrs;
    void clear() { addrs.clear(); }
};

namespace detail {

/// Header-only helper used by both `IRenderContext::build_draw_calls`
/// (defined in render_context.cpp) and the still-in-velk-scene
/// `BatchBuilder::build_gbuffer_draw_calls`. Writes a material's
/// draw-data to the frame buffer once per unique IProgram, returns
/// the cached GPU address on subsequent calls.
inline uint64_t write_material_once(IProgram* prog,
                                    IFrameDataManager& frame_data,
                                    ITextureResolver* resolver,
                                    MaterialAddrCache& cache)
{
    if (!prog) return 0;
    auto it = cache.addrs.find(prog);
    if (it != cache.addrs.end()) return it->second;

    uint64_t addr = 0;
    if (auto* dd = interface_cast<IDrawData>(prog)) {
        size_t sz = dd->get_draw_data_size();
        if (sz > 0 && (sz % 16) != 0) {
            VELK_LOG(E,
                     "Renderer: material get_draw_data_size (%zu) is not 16-byte aligned. "
                     "Use VELK_GPU_STRUCT for your material data.",
                     sz);
        }
        if (sz > 0) {
            void* scratch = std::malloc(sz);
            if (scratch) {
                std::memset(scratch, 0, sz);
                if (dd->write_draw_data(scratch, sz, resolver) == ReturnValue::Success) {
                    addr = frame_data.write(scratch, sz);
                }
                std::free(scratch);
            }
        }
    }
    cache.addrs[prog] = addr;
    return addr;
}

} // namespace detail

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
    MaterialAddrCache& material_cache,
    IBuffer* default_uv1,
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

        PipelineId pipeline = resolve_pipeline(batch);
        if (pipeline == 0) continue;

        // Per-batch persistent pool slice when available — a single
        // VkBuffer per view holds all batch slices, the slice was
        // bump-allocated and filled during BatchBuilder rebuild, and
        // transform-only frames write through the mapped pointer in
        // place. Falls through to per-frame staging for batches without
        // a slice (e.g. env_batch, which lives outside the pool).
        // Each batch owns its own GpuBufferHandle with the
        // [args(32)][count(16)][instance_data] layout. emit reads
        // args/count at offsets 0 and 32 of that buffer; the vertex
        // shader BDA-reads instances at storage_gpu_address + 48.
        // Batches that haven't been allocated a backing buffer yet
        // (env_batch, which lives outside the upload pipeline) fall
        // through to per-frame staging.
        const GpuBufferHandle storage_buffer = batch.storage_buffer();
        const bool has_storage = (storage_buffer != 0 && batch.storage_gpu_address() != 0);
        uint64_t instances_addr = 0;
        if (has_storage) {
            instances_addr = batch.storage_gpu_address() + BatchBufferLayout::kInstanceOffset;
        } else {
            auto instance_bytes = batch.instance_data();
            instances_addr =
                frame_data.write(instance_bytes.begin(), instance_bytes.size());
            if (!instances_addr) continue;
        }

        uint32_t texture_id = 0;
        if (batch.texture_key() != 0) {
            auto* tex = reinterpret_cast<ISurface*>(batch.texture_key());
            texture_id = resources.find_texture(tex);
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
        // vkCmdDraw when 0 (e.g. TriangleStrip unit quad).
        GpuBufferHandle ibo_handle = 0;
        size_t ibo_offset = 0;
        if (buffer->get_ibo_size() > 0) {
            auto* buf_entry = resources.find_buffer(buffer.get());
            if (!buf_entry || !buf_entry->handle) continue;
            ibo_handle = buf_entry->handle;
            ibo_offset = buffer->get_ibo_offset();
        }

        DrawDataHeader header{};
        header.instances_address = instances_addr;
        header.texture_id = texture_id;
        header.instance_count = batch.instance_count();
        header.vbo_address = buffer->get_gpu_handle(GpuResourceKey::Default);
        if (!header.vbo_address) continue;

        if (auto uv1 = primitive->get_uv1_buffer()) {
            uint64_t uv1_base = uv1->get_gpu_handle(GpuResourceKey::Default);
            if (!uv1_base) continue;
            header.uv1_address = uv1_base + primitive->get_uv1_offset();
            header.uv1_enabled = 1;
        } else {
            header.uv1_address = default_uv1
                ? default_uv1->get_gpu_handle(GpuResourceKey::Default)
                : 0;
            header.uv1_enabled = 0;
            if (!header.uv1_address) continue;
        }

        auto material_ptr = batch.material();
        uint64_t material_addr = detail::write_material_once(
            material_ptr.get(), frame_data,
            static_cast<ITextureResolver*>(&resources), material_cache);

        constexpr size_t kMaterialPtrSize = sizeof(uint64_t);
        size_t total_size = sizeof(DrawDataHeader) + kMaterialPtrSize;

        auto reservation = frame_data.reserve(total_size);
        if (!reservation.ptr) continue;

        auto* dst = static_cast<uint8_t*>(reservation.ptr);
        uint64_t draw_data_addr = reservation.gpu_addr;
        std::memcpy(dst, &header, sizeof(header));
        std::memcpy(dst + sizeof(DrawDataHeader), &material_addr, kMaterialPtrSize);

        // Lazy-register the program's pipeline for deferred destruction.
        // The manager subscribes itself as observer internally.
        if (material_ptr) {
            resources.register_pipeline(material_ptr.get(), pipeline);
        }

        // Always-indirect: pull args + count from the batch's own
        // storage buffer when available; fall back to writing a record
        // + uint32 count=1 into per-frame staging otherwise.
        DrawCall call{};
        call.pipeline = pipeline;
        call.indexed = (ibo_handle != 0);
        if (call.indexed) {
            call.index_buffer = ibo_handle;
            call.index_buffer_offset = ibo_offset;
            call.args_stride = sizeof(uint32_t) * 5; // VkDrawIndexedIndirectCommand
        } else {
            call.args_stride = sizeof(uint32_t) * 4; // VkDrawIndirectCommand
        }

        if (has_storage) {
            call.args_buffer        = storage_buffer;
            call.args_buffer_offset = BatchBufferLayout::kArgsOffset;
            call.count_buffer        = storage_buffer;
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
                uint64_t args_addr = frame_data.write(&args, sizeof(args));
                if (!args_addr) continue;
                call.args_buffer = frame_data.active_buffer();
                call.args_buffer_offset = args_addr - frame_data.active_buffer_base();
            } else {
                struct {
                    uint32_t vertexCount;
                    uint32_t instanceCount;
                    uint32_t firstVertex;
                    uint32_t firstInstance;
                } args{ primitive->get_vertex_count(), batch.instance_count(),
                        0, 0 };
                uint64_t args_addr = frame_data.write(&args, sizeof(args));
                if (!args_addr) continue;
                call.args_buffer = frame_data.active_buffer();
                call.args_buffer_offset = args_addr - frame_data.active_buffer_base();
            }
            uint32_t count_value = 1;
            uint64_t count_addr = frame_data.write(&count_value, sizeof(count_value), 4);
            if (!count_addr) continue;
            call.count_buffer = frame_data.active_buffer();
            call.count_buffer_offset = count_addr - frame_data.active_buffer_base();
        }
        call.max_draw_count = 1;

        call.root_constants_size = sizeof(uint64_t);
        std::memcpy(call.root_constants, &draw_data_addr, sizeof(uint64_t));

        out_calls.push_back(call);
    }
}

} // namespace velk

#endif // VELK_RENDER_FRAME_DRAW_CALL_EMIT_H
