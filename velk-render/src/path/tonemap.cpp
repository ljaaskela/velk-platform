#include "path/tonemap.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/api/cached_view_pass.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

#include <cstring>

namespace velk::impl {

namespace {

/// Compute shader source. Single dispatch reads `input_tex_id` via
/// bindless sampling, applies the ACES filmic curve, and writes to
/// `output_image_id` (a storage image). Operates per-pixel; 8x8 tile.
constexpr ::velk::string_view tonemap_compute_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Tonemap reads HDR (RGBA16F path target) and writes the clamped LDR
// result back into a RGBA16F storage target. The pipeline blits that
// to the swapchain's BGRA8 image as a single GPU op.
layout(set = 0, binding = 3, rgba16f) uniform image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint input_tex_id;
    uint output_image_id;
    uint width;
    uint height;
    float exposure;
    uint _pad0;
    uint _pad1;
    uint _pad2;
} pc;

vec3 tonemap_aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.width) || coord.y >= int(pc.height)) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.width), float(pc.height));
    vec4 src = velk_texture(pc.input_tex_id, uv);
    vec3 mapped = tonemap_aces(src.rgb * pc.exposure);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord,
               vec4(mapped, src.a));
}
)";

/// Stable compile-cache key for the tonemap pipeline. Bumped when the
/// shader source changes (here: added the exposure push constant).
/// Exposure itself is a runtime push constant, not a compile variant,
/// so a single pipeline still serves every exposure value.
constexpr uint64_t kTonemapPipelineKey = 0x546f6e656d617002ULL; // "Tonema\2"

} // namespace

::velk::IGpuPipeline::Ptr Tonemap::ensure_pipeline(::velk::FrameContext& ctx)
{
    if (!ctx.render_ctx) return {};
    // Weak cache is the source of truth: reuse if a live tonemap pass still
    // holds it, otherwise compile.
    if (auto p = ctx.render_ctx->find_pipeline(
            ::velk::PipelineCacheKey{kTonemapPipelineKey, ::velk::PixelFormat::RGBA8,
                                     ::velk::DepthFormat::None, 0})) {
        return p;
    }
    return ctx.render_ctx->compile_compute_pipeline(
        tonemap_compute_src, kTonemapPipelineKey);
}

void Tonemap::emit(::velk::IViewEntry& view,
                   ::velk::IRenderTarget::Ptr input,
                   ::velk::IRenderTarget::Ptr output,
                   ::velk::FrameContext& ctx,
                   ::velk::IRenderGraph& graph)
{
    if (!input || !output || !ctx.backend || !ctx.render_ctx) return;

    auto* in_surf = interface_cast<::velk::ISurface>(input.get());
    if (!in_surf) return;
    auto dims = in_surf->get_dimensions();
    if (dims.x == 0 || dims.y == 0) return;

    // Exposure lives on this effect's ITonemap state; read it through the
    // state system so main-thread writes are safely visible here.
    float exposure = 1.f;
    if (auto st = ::velk::read_state<::velk::ITonemap>(interface_cast<::velk::IObject>(this))) {
        exposure = st->exposure;
    }

    const uint32_t input_id =
        static_cast<uint32_t>(input->get_gpu_handle(::velk::GpuResourceKey::Default));
    const uint32_t output_id =
        static_cast<uint32_t>(output->get_gpu_handle(::velk::GpuResourceKey::Default));

    // Re-record only when something baked into the cached command buffer
    // changed (exposure toggle, input/output retarget, resize); otherwise the
    // cached pass is re-added as-is.
    auto& vs = view_states_[&view];
    bool dirty = !vs.snapshot_valid || vs.exposure != exposure
        || vs.input_id != input_id || vs.output_id != output_id || vs.dims != dims;

    // The tonemap compute shader uses no FrameGlobals, so the view-globals
    // address is irrelevant here (passed as 0).
    ::velk::emit_cached_view_pass(
        vs.pass, dirty, 0, graph, [&](::velk::CachedPassRecording& rec) {
            auto pipeline = ensure_pipeline(ctx);
            if (!pipeline) return;

            /// Push constant layout matches the shader's `PC` block above.
            /// Texture id reads come via bindless sampling; output is a
            /// storage image bound through the same descriptor table.
            VELK_GPU_STRUCT PushC {
                uint32_t input_tex_id;
                uint32_t output_image_id;
                uint32_t width;
                uint32_t height;
                float exposure;
                uint32_t _pad0;
                uint32_t _pad1;
                uint32_t _pad2;
            };
            static_assert(sizeof(PushC) == 32, "Tonemap PushC layout mismatch");

            PushC pc{};
            pc.input_tex_id    = input_id;
            pc.output_image_id = output_id;
            pc.width  = dims.x;
            pc.height = dims.y;
            pc.exposure = exposure;

            ::velk::DispatchCall dc{};
            dc.pipeline = pipeline.get();
            dc.groups_x = (dims.x + 7) / 8;
            dc.groups_y = (dims.y + 7) / 8;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(PushC);
            std::memcpy(dc.root_constants, &pc, sizeof(PushC));

            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("Tonemap");
                cmd->record_dispatch(dc);
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            // Hold the compute pipeline strong (the weak cache is not an owner).
            rec.held.push_back(std::move(pipeline));
            rec.reads.push_back(interface_pointer_cast<::velk::IGpuResource>(input));
            rec.writes.push_back(interface_pointer_cast<::velk::IGpuResource>(output));
        });

    vs.snapshot_valid = true;
    vs.exposure = exposure;
    vs.input_id = input_id;
    vs.output_id = output_id;
    vs.dims = dims;
}

void Tonemap::on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& /*ctx*/)
{
    view_states_.erase(&view);
}

void Tonemap::shutdown(::velk::FrameContext& /*ctx*/)
{
    view_states_.clear();
}

} // namespace velk::impl
