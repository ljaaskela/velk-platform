#ifndef VELK_RENDER_FRAME_INSTANCE_CULL_H
#define VELK_RENDER_FRAME_INSTANCE_CULL_H

#include <velk/string_view.h>
#include <velk/vector.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/interface/intf_batch.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/render_path/frame_context.h>

#include <algorithm>
#include <cstring>

namespace velk {

/**
 * Tests each instance's bounding sphere against the view and appends the visible
 * slots to its mesh's list in the culling arena, which the vertex shader reads through
 * velk_instance. The table at table_word holds 8 words per mesh: its bounding sphere
 * (center xyz and radius, as floats), the count of visible slots, the word of the
 * list, and the list's capacity. Instances with a zero matrix have been removed.
 */
inline constexpr string_view instance_cull_compute_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

struct CullInstance {
    mat4 world_matrix;
    vec4 offset;
    vec4 size;
    vec4 color;
    uvec4 params;
};

layout(set = 1, binding = 3, scalar) readonly buffer CullInstances { CullInstance data[]; } cull_instances;
layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint instances_base;
    uint slot_count;
    uint table_word;
    uint globals_base;
    float max_distance;
} pc;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= pc.slot_count) return;

    mat4 world = cull_instances.data[pc.instances_base + slot].world_matrix;
    if (world[3][3] == 0.0) return;

    uint table = pc.table_word + cull_instances.data[pc.instances_base + slot].params.y * 8u;
    vec4 sphere = vec4(uintBitsToFloat(cull_words.data[table + 0u]),
                       uintBitsToFloat(cull_words.data[table + 1u]),
                       uintBitsToFloat(cull_words.data[table + 2u]),
                       uintBitsToFloat(cull_words.data[table + 3u]));
    vec3 center = (world * vec4(sphere.xyz, 1.0)).xyz;
    float scale2 = max(max(dot(world[0].xyz, world[0].xyz), dot(world[1].xyz, world[1].xyz)),
                       dot(world[2].xyz, world[2].xyz));
    float radius = sphere.w * sqrt(scale2);

    GlobalData globals = velk_globals.data[pc.globals_base];
    if (pc.max_distance > 0.0 && length(center - globals.cam_pos.xyz) - radius > pc.max_distance) return;

    // The sphere against each clip plane. Planes are rows of the matrix added to or
    // taken from its last row, so their normals' lengths are at most the sum of the
    // rows' lengths, which keeps this conservative. Near and far are tested with the
    // wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 clip = vp * vec4(center, 1.0);
    vec3 r0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    vec3 r1 = vec3(vp[0][1], vp[1][1], vp[2][1]);
    vec3 r2 = vec3(vp[0][2], vp[1][2], vp[2][2]);
    vec3 r3 = vec3(vp[0][3], vp[1][3], vp[2][3]);
    float w_reach = radius * length(r3);
    if (abs(clip.x) > clip.w + w_reach + radius * length(r0)) return;
    if (abs(clip.y) > clip.w + w_reach + radius * length(r1)) return;
    if (abs(clip.z) > clip.w + w_reach + radius * length(r2)) return;

    uint index = atomicAdd(cull_words.data[table + 4u], 1u);
    if (index < cull_words.data[table + 6u]) {
        cull_words.data[cull_words.data[table + 5u] + index] = slot;
    }
}
)";

/**
 * @brief One view's GPU culling pass, kept by the render path. Set @c dirty when the
 *        view's batches change.
 */
struct InstanceCullPass
{
    IRenderPass::Ptr pass;
    bool dirty = true;
    vector<uint32_t> count_words;  ///< Reset before every cull.
    IGpuBuffer* arena_buffer = nullptr;
};

/**
 * @brief Adds @p batches' GPU culling to @p graph, if any of them are culled. Runs
 *        before the passes that draw them.
 */
inline void emit_instance_cull_pass(InstanceCullPass& state, const vector<IBatch::Ptr>* batches,
                                    uint32_t view_globals_base, FrameContext& ctx, IRenderGraph& graph)
{
    if (!batches || !ctx.resources || !ctx.backend || !ctx.render_ctx) {
        return;
    }

    bool record = false;
    if (state.dirty) {
        state.dirty = false;
        record = true;
        state.count_words.clear();
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info)
                && std::find(state.count_words.begin(), state.count_words.end(), info.count_word)
                       == state.count_words.end()) {
                state.count_words.push_back(info.count_word);
            }
        }
    }
    if (state.count_words.empty()) {
        return;
    }

    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    IGpuBuffer* arena_buffer = arena ? arena->buffer() : nullptr;
    if (!arena_buffer) {
        return;
    }
    // The copies below name the arena's buffer, which growing it replaces.
    record = record || arena_buffer != state.arena_buffer;
    state.arena_buffer = arena_buffer;

    const uint32_t zero = 0;
    for (uint32_t word : state.count_words) {
        arena->update_at(uint64_t(word) * sizeof(uint32_t), &zero, sizeof(zero));
    }

    emit_cached_view_pass(state.pass, record, "instance.cull", graph, [&](CachedPassRecording& rec) {
        constexpr uint64_t key = 0x4375'6c6c'496e'7374ULL;
        auto pipeline = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
        if (!pipeline) {
            pipeline = ctx.render_ctx->pipelines().compile_compute(instance_cull_compute_src, key);
        }
        auto cmd = pipeline ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }

        cmd->begin_recording();
        cmd->push_label("Instance culling");
        // Earlier frames may still be drawing from these lists.
        cmd->record_global_barrier();

        struct PushC
        {
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
        };
        vector<const void*> done;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info)
                || std::find(done.begin(), done.end(), info.key) != done.end()) {
                continue;
            }
            done.push_back(info.key);

            const PushC pc{info.instances_base, info.slot_count, info.table_word, view_globals_base,
                           info.max_distance};
            DispatchCall dc{};
            dc.pipeline = pipeline.get();
            dc.groups_x = (info.slot_count + 63u) / 64u;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            cmd->record_dispatch(dc);
        }
        cmd->record_global_barrier();

        // Each batch's indirect args take the count of its list.
        constexpr uint64_t kInstanceCountOffset = BatchBufferLayout::kArgsOffset + sizeof(uint32_t);
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (!batch || !batch->get_instance_cull(info)) {
                continue;
            }
            auto* entry = batch->storage_buffer() ? ctx.resources->find_buffer(batch->storage_buffer()) : nullptr;
            auto args = entry ? entry->buffer.lock() : nullptr;
            if (args) {
                cmd->record_copy_buffer(arena_buffer, uint64_t(info.count_word) * sizeof(uint32_t), args.get(),
                                        kInstanceCountOffset, sizeof(uint32_t));
            }
        }
        cmd->record_global_barrier();

        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.held.push_back(std::move(pipeline));
    });
}

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_H
