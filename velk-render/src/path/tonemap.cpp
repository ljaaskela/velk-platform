#include "path/tonemap.h"

#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/gpu_data.h>
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
    GlobalData globals;        // [0..8) push_view_globals (unused here)
    uint input_tex_id;         // [8..)  CPU push starts here
    uint output_image_id;
    uint width;
    uint height;
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
    vec3 mapped = tonemap_aces(src.rgb);
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord,
               vec4(mapped, src.a));
}
)";

/// Stable compile-cache key for the tonemap pipeline. ACES is the
/// only variant today; if user-tunable parameters add variants the
/// key gains a hash of those.
constexpr uint64_t kTonemapPipelineKey = 0x546f6e656d617001ULL; // "Tonema\1"

} // namespace

uint64_t Tonemap::ensure_pipeline(::velk::FrameContext& ctx)
{
    if (compiled_) return kTonemapPipelineKey;
    if (!ctx.render_ctx) return 0;

    uint64_t handle = ctx.render_ctx->compile_compute_pipeline(
        tonemap_compute_src, kTonemapPipelineKey);
    if (handle == 0) return 0;

    compiled_ = true;
    return kTonemapPipelineKey;
}

void Tonemap::emit(::velk::IViewEntry& /*view*/,
                   ::velk::IRenderTarget::Ptr input,
                   ::velk::IRenderTarget::Ptr output,
                   ::velk::FrameContext& ctx,
                   ::velk::IRenderGraph& graph)
{
    if (!input || !output || !ctx.backend || !ctx.pipeline_map) return;

    auto* in_surf = interface_cast<::velk::ISurface>(input.get());
    if (!in_surf) return;
    auto dims = in_surf->get_dimensions();
    if (dims.x == 0 || dims.y == 0) return;

    uint64_t pipeline_key = ensure_pipeline(ctx);
    if (pipeline_key == 0) return;
    auto pit = ctx.pipeline_map->find(
        ::velk::PipelineCacheKey{pipeline_key, ::velk::PixelFormat::Surface, 0});
    if (pit == ctx.pipeline_map->end()) return;

    /// Push constant layout matches the shader's `PC` block above.
    /// Texture id reads come via bindless sampling; output is a
    /// storage image bound through the same descriptor table.
    VELK_GPU_STRUCT PushC {
        uint32_t input_tex_id;
        uint32_t output_image_id;
        uint32_t width;
        uint32_t height;
    };
    static_assert(sizeof(PushC) == 16, "Tonemap PushC layout mismatch");

    PushC pc{};
    pc.input_tex_id =
        static_cast<uint32_t>(input->get_gpu_handle(::velk::GpuResourceKey::Default));
    pc.output_image_id =
        static_cast<uint32_t>(output->get_gpu_handle(::velk::GpuResourceKey::Default));
    pc.width = dims.x;
    pc.height = dims.y;

    ::velk::DispatchCall dc{};
    dc.pipeline = pit->second;
    dc.groups_x = (dims.x + 7) / 8;
    dc.groups_y = (dims.y + 7) / 8;
    dc.groups_z = 1;
    dc.root_constants_size = sizeof(PushC);
    std::memcpy(dc.root_constants, &pc, sizeof(PushC));

    auto gp = ::velk::instance().create<::velk::IRenderPass>(::velk::ClassId::DefaultRenderPass);
    if (!gp) return;
    gp->add_op(::velk::ops::Dispatch{dc});
    gp->add_read(interface_pointer_cast<::velk::IGpuResource>(input));
    gp->add_write(interface_pointer_cast<::velk::IGpuResource>(output));
    graph.add_pass(std::move(gp));
}

} // namespace velk::impl
