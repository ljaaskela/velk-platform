#include "path/bloom.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>

#include <cstring>

namespace velk::impl {

namespace {

/// Soft-knee prefilter + 13-tap downsample. The first downsample folds the
/// threshold in (`prefilter != 0`); later mips skip it. Source texel size is
/// passed explicitly (`in_w`/`in_h`) because odd dimensions don't halve cleanly.
constexpr ::velk::string_view bloom_downsample_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 3, rgba16f) uniform image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint  input_tex_id;
    uint  output_image_id;
    uint  out_w;
    uint  out_h;
    uint  in_w;
    uint  in_h;
    uint  prefilter;
    float threshold;
    float knee;
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
} pc;

// Quadratic soft-knee around the threshold (Unreal / COD style): below the
// knee nothing passes, across the knee the rolloff is smooth, above it the
// full pixel passes. Scaled by the per-channel max so colored emitters keep
// their hue.
vec3 bloom_prefilter(vec3 c)
{
    float br   = max(c.r, max(c.g, c.b));
    float kn   = pc.knee * pc.threshold + 1e-5;
    float soft = clamp(br - pc.threshold + kn, 0.0, 2.0 * kn);
    soft       = soft * soft / (4.0 * kn + 1e-5);
    float contrib = max(soft, br - pc.threshold) / max(br, 1e-5);
    return c * contrib;
}

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.out_w) || coord.y >= int(pc.out_h)) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.out_w), float(pc.out_h));
    vec2 t  = 1.0 / vec2(float(pc.in_w), float(pc.in_h));

    vec3 a = velk_texture(pc.input_tex_id, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 b = velk_texture(pc.input_tex_id, uv + t * vec2( 0.0, -2.0)).rgb;
    vec3 c = velk_texture(pc.input_tex_id, uv + t * vec2( 2.0, -2.0)).rgb;
    vec3 d = velk_texture(pc.input_tex_id, uv + t * vec2(-2.0,  0.0)).rgb;
    vec3 e = velk_texture(pc.input_tex_id, uv).rgb;
    vec3 f = velk_texture(pc.input_tex_id, uv + t * vec2( 2.0,  0.0)).rgb;
    vec3 g = velk_texture(pc.input_tex_id, uv + t * vec2(-2.0,  2.0)).rgb;
    vec3 h = velk_texture(pc.input_tex_id, uv + t * vec2( 0.0,  2.0)).rgb;
    vec3 i = velk_texture(pc.input_tex_id, uv + t * vec2( 2.0,  2.0)).rgb;
    vec3 j = velk_texture(pc.input_tex_id, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 k = velk_texture(pc.input_tex_id, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 l = velk_texture(pc.input_tex_id, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 m = velk_texture(pc.input_tex_id, uv + t * vec2( 1.0,  1.0)).rgb;

    vec3 col = e * 0.125;
    col += (a + c + g + i) * 0.03125;
    col += (b + d + f + h) * 0.0625;
    col += (j + k + l + m) * 0.125;

    if (pc.prefilter != 0u) col = bloom_prefilter(col);

    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(col, 1.0));
}
)";

/// 3x3 tent-filter upsample of the smaller mip (`low`), added to the larger
/// downsample mip (`high`) and written to the larger-resolution output. `high`
/// and `output` are different textures, so there is no in-place read/write
/// hazard; the render graph barriers the dependency between passes.
constexpr ::velk::string_view bloom_upsample_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 3, rgba16f) uniform image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint  low_tex_id;
    uint  high_tex_id;
    uint  output_image_id;
    uint  out_w;
    uint  out_h;
    float radius;
    uint  _pad0;
    uint  _pad1;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.out_w) || coord.y >= int(pc.out_h)) return;

    vec2 uv = (vec2(coord) + 0.5) / vec2(float(pc.out_w), float(pc.out_h));
    vec2 t  = (1.0 / vec2(float(pc.out_w), float(pc.out_h))) * pc.radius;

    vec3 s  = velk_texture(pc.low_tex_id, uv + t * vec2(-1.0, -1.0)).rgb * 1.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2( 0.0, -1.0)).rgb * 2.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2( 1.0, -1.0)).rgb * 1.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2(-1.0,  0.0)).rgb * 2.0;
    s      += velk_texture(pc.low_tex_id, uv).rgb                         * 4.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2( 1.0,  0.0)).rgb * 2.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2(-1.0,  1.0)).rgb * 1.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2( 0.0,  1.0)).rgb * 2.0;
    s      += velk_texture(pc.low_tex_id, uv + t * vec2( 1.0,  1.0)).rgb * 1.0;
    s      *= (1.0 / 16.0);

    vec3 high = velk_texture(pc.high_tex_id, uv).rgb;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord, vec4(high + s, 1.0));
}
)";

/// Final composite: full-res scene + intensity * glow. Reads the half-res glow
/// with bilinear upscaling; keeps the scene's alpha so the downstream tonemap
/// and surface composite behave unchanged.
constexpr ::velk::string_view bloom_combine_src = R"(
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "velk.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 3, rgba16f) uniform image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint  input_tex_id;
    uint  bloom_tex_id;
    uint  output_image_id;
    uint  out_w;
    uint  out_h;
    float intensity;
    uint  _pad0;
    uint  _pad1;
} pc;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(pc.out_w) || coord.y >= int(pc.out_h)) return;

    vec2 uv  = (vec2(coord) + 0.5) / vec2(float(pc.out_w), float(pc.out_h));
    vec4 src = velk_texture(pc.input_tex_id, uv);
    vec3 glow = velk_texture(pc.bloom_tex_id, uv).rgb;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_image_id)], coord,
               vec4(src.rgb + pc.intensity * glow, src.a));
}
)";

// Stable compile-cache keys, one per bloom pipeline. Bump the trailing byte
// when a shader source changes (runtime recompile picks it up either way; the
// key only dedups live pipelines within a run).
constexpr uint64_t kBloomDownsampleKey = 0x426c6f6f6d44'01ULL; // "BloomD\1"
constexpr uint64_t kBloomUpsampleKey   = 0x426c6f6f6d55'01ULL; // "BloomU\1"
constexpr uint64_t kBloomCombineKey    = 0x426c6f6f6d43'01ULL; // "BloomC\1"

::velk::IGpuPipeline::Ptr ensure(::velk::FrameContext& ctx, uint64_t key,
                                 ::velk::string_view src)
{
    if (!ctx.render_ctx) return {};
    if (auto p = ctx.render_ctx->find_pipeline(
            ::velk::PipelineCacheKey{key, ::velk::PixelFormat::RGBA8,
                                     ::velk::DepthFormat::None, 0})) {
        return p;
    }
    return ctx.render_ctx->compile_compute_pipeline(src, key);
}

} // namespace

::velk::IGpuPipeline::Ptr Bloom::ensure_downsample_pipeline(::velk::FrameContext& ctx)
{
    return ensure(ctx, kBloomDownsampleKey, bloom_downsample_src);
}

::velk::IGpuPipeline::Ptr Bloom::ensure_upsample_pipeline(::velk::FrameContext& ctx)
{
    return ensure(ctx, kBloomUpsampleKey, bloom_upsample_src);
}

::velk::IGpuPipeline::Ptr Bloom::ensure_combine_pipeline(::velk::FrameContext& ctx)
{
    return ensure(ctx, kBloomCombineKey, bloom_combine_src);
}

Bloom::ViewState* Bloom::ensure_chain(::velk::IViewEntry& view, int width, int height,
                                      ::velk::IRenderGraph& graph)
{
    // Mip dimensions: start at half resolution and halve until a mip would
    // drop below 8 px or we hit the iteration cap. Wider chains spread the
    // glow further; 6 mips is plenty for 720p-class targets.
    constexpr int kMaxMips = 6;
    constexpr int kMinDim  = 8;
    ::velk::vector<::velk::uvec2> dims;
    int w = width / 2;
    int h = height / 2;
    for (int i = 0; i < kMaxMips && w >= kMinDim && h >= kMinDim; ++i) {
        dims.push_back({static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
        w /= 2;
        h /= 2;
    }
    if (dims.empty()) return nullptr; // target too small to bloom

    auto& vs = view_states_[&view];
    ::velk::uvec2 want{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (vs.size == want && vs.down.size() == dims.size()) {
        return &vs;
    }

    vs.down.clear();
    vs.up.clear();
    vs.size = want;

    auto make = [&graph](::velk::uvec2 d) {
        ::velk::TextureDesc td{};
        td.width  = static_cast<int>(d.x);
        td.height = static_cast<int>(d.y);
        td.format = ::velk::PixelFormat::RGBA16F; // HDR; bloom runs before tonemap
        td.usage  = ::velk::TextureUsage::Storage;
        return graph.resources().create_render_texture(td);
    };

    for (auto& d : dims) {
        auto tex = make(d);
        if (!tex) { vs.down.clear(); vs.up.clear(); vs.size = {}; return nullptr; }
        vs.down.push_back(std::move(tex));
    }
    // One upsample accumulator per mip except the smallest (which is its own
    // starting point in the upsample chain).
    for (size_t i = 0; i + 1 < dims.size(); ++i) {
        auto tex = make(dims[i]);
        if (!tex) { vs.down.clear(); vs.up.clear(); vs.size = {}; return nullptr; }
        vs.up.push_back(std::move(tex));
    }
    return &vs;
}

void Bloom::emit(::velk::IViewEntry& view,
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

    // Effect parameters come off this effect's IBloom state (main-thread
    // writes made visible here through the state system).
    float threshold = 1.f, knee = 0.5f, intensity = 0.05f, radius = 1.f;
    if (auto st = ::velk::read_state<::velk::IBloom>(interface_cast<::velk::IObject>(this))) {
        threshold = st->threshold;
        knee      = st->knee;
        intensity = st->intensity;
        radius    = st->radius;
    }

    auto& vs = view_states_[&view];

    const uint32_t input_id = static_cast<uint32_t>(
        input->get_gpu_handle(::velk::GpuResourceKey::Default));
    const uint32_t out_tex_id = static_cast<uint32_t>(
        output->get_gpu_handle(::velk::GpuResourceKey::Default));
    const bool passthrough = intensity <= 0.f;

    // Re-record only when something baked into the cached command buffers
    // changed: target size (resize), input/output retarget, the
    // passthrough-vs-chain structure, or (chain only) a tunable param toggle.
    // Otherwise re-add the cached passes verbatim, so the render graph's
    // compile short-circuit also holds in steady state.
    const bool rebuild = !vs.snapshot_valid || vs.passes.empty()
        || vs.dims != dims
        || vs.input_id != input_id || vs.output_id != out_tex_id
        || vs.passthrough != passthrough
        || (!passthrough && (vs.threshold != threshold || vs.knee != knee
                             || vs.intensity != intensity || vs.radius != radius));

    if (!rebuild) {
        for (auto& p : vs.passes) graph.add_pass(p);
        return;
    }

    vs.passes.clear();

    // Helper: record one compute dispatch as a cached render-graph pass. The
    // graph orders passes from the declared reads/writes, inserting the
    // barriers between dependent bloom stages (no per-dispatch barrier API).
    auto emit_pass = [&](const char* label, ::velk::IGpuPipeline::Ptr pipe,
                         ::velk::uvec2 groups_dims,
                         const void* pc, size_t pc_size,
                         std::initializer_list<::velk::IRenderTarget::Ptr> reads,
                         const ::velk::IRenderTarget::Ptr& write) {
        if (!pipe) return;
        ::velk::DispatchCall dc{};
        dc.pipeline = pipe.get();
        dc.groups_x = (groups_dims.x + 7) / 8;
        dc.groups_y = (groups_dims.y + 7) / 8;
        dc.groups_z = 1;
        dc.root_constants_size = static_cast<uint32_t>(pc_size);
        std::memcpy(dc.root_constants, pc, pc_size);

        auto gp = ::velk::instance().create<::velk::IRenderPass>(
            ::velk::ClassId::DefaultRenderPass);
        if (!gp) return;
        gp->set_name("bloom");
        if (auto cmd = ctx.backend->create_command_buffer()) {
            cmd->begin_recording();
            cmd->push_label(label);
            cmd->record_dispatch(dc);
            cmd->pop_label();
            cmd->end_recording();
            gp->set_command_buffer(std::move(cmd));
        }
        for (auto& r : reads) {
            gp->add_read(interface_pointer_cast<::velk::IGpuResource>(r));
        }
        gp->add_write(interface_pointer_cast<::velk::IGpuResource>(write));
        ::velk::vector<::velk::IGpuPipeline::Ptr> held;
        held.push_back(std::move(pipe));
        gp->set_held_pipelines(std::move(held));
        vs.passes.push_back(gp);
        graph.add_pass(gp);
    };

    // Passthrough: copy the HDR input into the intermediate the downstream
    // tonemap reads. Used when bloom is off (intensity 0, e.g. daytime) or the
    // target is too small to build a mip chain, so the post chain still
    // produces the right output.
    auto record_passthrough = [&]() {
        auto gp = ::velk::instance().create<::velk::IRenderPass>(
            ::velk::ClassId::DefaultRenderPass);
        if (!gp) return;
        gp->set_name("bloom");
        ::velk::IGpuTexture* in_tex  = graph.resources().find_texture(input.get());
        ::velk::IGpuTexture* out_tex = graph.resources().find_texture(output.get());
        if (in_tex && out_tex) {
            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("Bloom: passthrough");
                // Empty rect = full dst extent (backend fills it); a 1:1 copy.
                cmd->record_blit_to_texture(*in_tex, *out_tex, {});
                cmd->pop_label();
                cmd->end_recording();
                gp->set_command_buffer(std::move(cmd));
            }
        }
        gp->add_read(interface_pointer_cast<::velk::IGpuResource>(input));
        gp->add_write(interface_pointer_cast<::velk::IGpuResource>(output));
        vs.passes.push_back(gp);
        graph.add_pass(gp);
    };

    if (passthrough) {
        record_passthrough();
    } else if (auto* chain = ensure_chain(view, static_cast<int>(dims.x),
                                          static_cast<int>(dims.y), graph);
               !chain || chain->down.empty()) {
        record_passthrough(); // target too small to bloom
    } else {
        const size_t n = chain->down.size();

        // Push-constant layouts mirror each shader's PC block.
        VELK_GPU_STRUCT DownPC {
            uint32_t input_tex_id, output_image_id, out_w, out_h, in_w, in_h, prefilter;
            float    threshold, knee;
            uint32_t _pad0, _pad1, _pad2;
        };
        static_assert(sizeof(DownPC) == 48, "Bloom DownPC layout mismatch");
        VELK_GPU_STRUCT UpPC {
            uint32_t low_tex_id, high_tex_id, output_image_id, out_w, out_h;
            float    radius;
            uint32_t _pad0, _pad1;
        };
        static_assert(sizeof(UpPC) == 32, "Bloom UpPC layout mismatch");
        VELK_GPU_STRUCT CombinePC {
            uint32_t input_tex_id, bloom_tex_id, output_image_id, out_w, out_h;
            float    intensity;
            uint32_t _pad0, _pad1;
        };
        static_assert(sizeof(CombinePC) == 32, "Bloom CombinePC layout mismatch");

        auto tex_id = [](const ::velk::IRenderTarget::Ptr& t) {
            return static_cast<uint32_t>(t->get_gpu_handle(::velk::GpuResourceKey::Default));
        };
        auto tex_dims = [](const ::velk::IRenderTarget::Ptr& t) -> ::velk::uvec2 {
            if (auto* s = interface_cast<::velk::ISurface>(t.get())) return s->get_dimensions();
            return {};
        };

        // Downsample chain: input -> down[0] (prefilter) -> down[1] -> ...
        for (size_t i = 0; i < n; ++i) {
            ::velk::uvec2 od = tex_dims(chain->down[i]);
            ::velk::uvec2 id_dims = (i == 0) ? dims : tex_dims(chain->down[i - 1]);
            DownPC pc{};
            pc.input_tex_id    = (i == 0) ? input_id : tex_id(chain->down[i - 1]);
            pc.output_image_id = tex_id(chain->down[i]);
            pc.out_w = od.x; pc.out_h = od.y;
            pc.in_w  = id_dims.x; pc.in_h = id_dims.y;
            pc.prefilter = (i == 0) ? 1u : 0u;
            pc.threshold = threshold;
            pc.knee      = knee;
            emit_pass("Bloom: downsample", ensure_downsample_pipeline(ctx), od,
                      &pc, sizeof(pc),
                      {(i == 0) ? input : chain->down[i - 1]}, chain->down[i]);
        }

        // Upsample chain: up[n-2] = down[n-1] (+) down[n-2], then down to up[0].
        for (size_t k = n - 1; k-- > 0; ) { // k = n-2 .. 0
            ::velk::uvec2 od = tex_dims(chain->down[k]);
            const ::velk::IRenderTarget::Ptr& low =
                (k + 1 == n - 1) ? chain->down[n - 1] : chain->up[k + 1];
            UpPC pc{};
            pc.low_tex_id     = tex_id(low);
            pc.high_tex_id    = tex_id(chain->down[k]);
            pc.output_image_id = tex_id(chain->up[k]);
            pc.out_w = od.x; pc.out_h = od.y;
            pc.radius = radius;
            emit_pass("Bloom: upsample", ensure_upsample_pipeline(ctx), od,
                      &pc, sizeof(pc), {low, chain->down[k]}, chain->up[k]);
        }

        // Combine: scene + intensity * glow. Glow source is up[0] (full chain)
        // or down[0] when the chain is a single mip (no upsample produced).
        const ::velk::IRenderTarget::Ptr& glow = (n >= 2) ? chain->up[0] : chain->down[0];
        CombinePC pc{};
        pc.input_tex_id    = input_id;
        pc.bloom_tex_id    = tex_id(glow);
        pc.output_image_id = out_tex_id;
        pc.out_w = dims.x; pc.out_h = dims.y;
        pc.intensity = intensity;
        emit_pass("Bloom: combine", ensure_combine_pipeline(ctx), dims,
                  &pc, sizeof(pc), {input, glow}, output);
    }

    // Snapshot what was baked, for next frame's change detection.
    vs.snapshot_valid = true;
    vs.dims = dims;
    vs.threshold = threshold;
    vs.knee = knee;
    vs.intensity = intensity;
    vs.radius = radius;
    vs.input_id = input_id;
    vs.output_id = out_tex_id;
    vs.passthrough = passthrough;
}

void Bloom::on_view_removed(::velk::IViewEntry& view, ::velk::FrameContext& /*ctx*/)
{
    view_states_.erase(&view);
}

void Bloom::shutdown(::velk::FrameContext& /*ctx*/)
{
    view_states_.clear();
}

} // namespace velk::impl
