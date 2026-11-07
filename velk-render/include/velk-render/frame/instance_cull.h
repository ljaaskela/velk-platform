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

/// Slots in a chunk of a partition's list. A partition's last chunk is filled out with
/// kCullNoSlot.
inline constexpr uint32_t kCullChunkSlots = 64;
inline constexpr uint32_t kCullNoSlot = 0xFFFFFFFFu;
/// Words per partition in a partition table. See instance_partition_cull_src.
inline constexpr uint32_t kCullEntryWords = 8;
/// Words before the chunks in a chunk list. See instance_partition_cull_src.
inline constexpr uint32_t kCullChunkListHeaderWords = 4;

/**
 * Tests a visual's partitions against the view and appends the chunks of those that
 * pass to a list, for instance_cull_compute_src to go through. A table entry holds the
 * partition's world bounds (min xyz and max xyz, as floats), the word of its first
 * chunk, and its chunk count, 0 for an unused entry. The list at list_word starts with
 * the group counts of the dispatch that follows, and its first word is the chunk count.
 */
inline constexpr string_view instance_partition_cull_src = R"(
#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;

layout(set = 1, binding = 16, std430) buffer CullWords { uint data[]; } cull_words;

layout(push_constant) uniform PC {
    uint table_word;
    uint entry_count;
    uint list_word;
    uint globals_base;
    float max_distance;
} pc;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= pc.entry_count) return;

    uint entry = pc.table_word + index * 8u;
    uint chunks = cull_words.data[entry + 7u];
    if (chunks == 0u) return;

    vec3 lo = vec3(uintBitsToFloat(cull_words.data[entry + 0u]), uintBitsToFloat(cull_words.data[entry + 1u]),
                   uintBitsToFloat(cull_words.data[entry + 2u]));
    vec3 hi = vec3(uintBitsToFloat(cull_words.data[entry + 3u]), uintBitsToFloat(cull_words.data[entry + 4u]),
                   uintBitsToFloat(cull_words.data[entry + 5u]));

    GlobalData globals = velk_globals.data[pc.globals_base];
    if (pc.max_distance > 0.0) {
        vec3 away = max(max(lo - globals.cam_pos.xyz, globals.cam_pos.xyz - hi), vec3(0.0));
        if (length(away) > pc.max_distance) return;
    }

    // Outside if the corner furthest along a plane's normal is behind it. Near and far
    // use the wider of the two conventions for clip z.
    mat4 vp = globals.view_projection;
    vec4 r0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 r1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 r2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 r3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    vec4 planes[6] = vec4[6](r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2);
    for (int p = 0; p < 6; ++p) {
        vec3 corner = mix(lo, hi, greaterThanEqual(planes[p].xyz, vec3(0.0)));
        if (dot(planes[p].xyz, corner) + planes[p].w < 0.0) return;
    }

    uint first = cull_words.data[entry + 6u];
    uint at = atomicAdd(cull_words.data[pc.list_word], chunks);
    for (uint c = 0u; c < chunks; ++c) {
        cull_words.data[pc.list_word + 4u + at + c] = first + c * 64u;
    }
}
)";

/**
 * Tests each instance's bounding sphere against the view and appends the visible
 * slots to its mesh's list in the culling arena, which the vertex shader reads through
 * velk_instance. The table at table_word holds 8 words per mesh: its bounding sphere
 * (center xyz and radius, as floats), the count of visible slots, the word of the
 * list, and the list's capacity. Instances with a zero matrix have been removed.
 *
 * With chunks_word set, each work group goes through the chunk at that index of the
 * list, as filled by instance_partition_cull_src. Without it, all slots are tested.
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
    uint chunks_word;
} pc;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (pc.chunks_word != 0xFFFFFFFFu) {
        slot = cull_words.data[cull_words.data[pc.chunks_word + gl_WorkGroupID.x] + gl_LocalInvocationIndex];
    }
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
    uint64_t partition_tables = 0;  ///< Which tables the recording uses.
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

    // The BVH replaces a visual's tables when they fill up.
    uint64_t partition_tables = 0;
    for (const auto& batch : *batches) {
        InstanceCullInfo info{};
        if (batch && batch->get_instance_cull(info)) {
            partition_tables = partition_tables * 31u + info.partition_table_word;
            partition_tables = partition_tables * 31u + info.partition_count;
            partition_tables = partition_tables * 31u + info.chunk_list_word;
        }
    }
    record = record || partition_tables != state.partition_tables;
    state.partition_tables = partition_tables;

    const uint32_t zero = 0;
    for (uint32_t word : state.count_words) {
        arena->update_at(uint64_t(word) * sizeof(uint32_t), &zero, sizeof(zero));
    }

    emit_cached_view_pass(state.pass, record, "instance.cull", graph, [&](CachedPassRecording& rec) {
        const auto pipeline_for = [&](string_view src, uint64_t key) {
            auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
            return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
        };
        auto pipeline = pipeline_for(instance_cull_compute_src, 0x4375'6c6c'496e'7374ULL);
        auto partition_pipeline = pipeline_for(instance_partition_cull_src, 0x4375'6c6c'5061'7274ULL);
        auto cmd = pipeline && partition_pipeline ? ctx.backend->create_command_buffer() : nullptr;
        if (!cmd) {
            return;
        }

        cmd->begin_recording();
        cmd->push_label("Instance culling");
        // Earlier frames may still be drawing from these lists.
        cmd->record_global_barrier();

        vector<const void*> done;
        vector<InstanceCullInfo> visuals;
        for (const auto& batch : *batches) {
            InstanceCullInfo info{};
            if (batch && batch->get_instance_cull(info)
                && std::find(done.begin(), done.end(), info.key) == done.end()) {
                done.push_back(info.key);
                visuals.push_back(info);
            }
        }

        // Partitions first, so whole partitions out of view cost nothing per instance.
        bool any_partitions = false;
        for (const auto& info : visuals) {
            if (info.partition_count != 0) {
                any_partitions = true;
                cmd->record_clear_buffer(arena_buffer, uint64_t(info.chunk_list_word) * sizeof(uint32_t), sizeof(uint32_t));
            }
        }
        if (any_partitions) {
            cmd->record_global_barrier();
            struct PartitionPushC
            {
                uint32_t table_word;
                uint32_t entry_count;
                uint32_t list_word;
                uint32_t globals_base;
                float max_distance;
            };
            for (const auto& info : visuals) {
                if (info.partition_count == 0) {
                    continue;
                }
                const PartitionPushC pc{info.partition_table_word, info.partition_count, info.chunk_list_word,
                                        view_globals_base, info.max_distance};
                DispatchCall dc{};
                dc.pipeline = partition_pipeline.get();
                dc.groups_x = (info.partition_count + 63u) / 64u;
                dc.groups_y = 1;
                dc.groups_z = 1;
                dc.root_constants_size = sizeof(pc);
                std::memcpy(dc.root_constants, &pc, sizeof(pc));
                cmd->record_dispatch(dc);
            }
            cmd->record_global_barrier();
        }

        struct PushC
        {
            uint32_t instances_base;
            uint32_t slot_count;
            uint32_t table_word;
            uint32_t globals_base;
            float max_distance;
            uint32_t chunks_word;
        };
        for (const auto& info : visuals) {
            const bool partitioned = info.partition_count != 0;
            const PushC pc{info.instances_base, info.slot_count, info.table_word, view_globals_base, info.max_distance,
                           partitioned ? info.chunk_list_word + kCullChunkListHeaderWords : kCullNoSlot};
            DispatchCall dc{};
            dc.pipeline = pipeline.get();
            dc.groups_x = (info.slot_count + 63u) / 64u;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            if (partitioned) {
                cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(info.chunk_list_word) * sizeof(uint32_t));
            } else {
                cmd->record_dispatch(dc);
            }
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
        rec.held.push_back(std::move(partition_pipeline));
    });
}

} // namespace velk

#endif // VELK_RENDER_FRAME_INSTANCE_CULL_H
