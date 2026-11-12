#include "path/deferred_path.h"
#include "path/material_pipeline.h"

#include <velk-render/api/cached_view_pass.h>

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/string.h>

#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

#include <velk-render/frame/compute_shaders.h>
#include <velk-render/frame/draw_call_emit.h>
#include <velk-render/frame/raster_shaders.h>
#include "path/deferred_gbuffer.h"
#include "path/ltc_table.h"
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_light.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/material/intf_material.h>

#include <velk/api/state.h>
#include <velk/interface/intf_object.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velk {

namespace {

/// The pipeline writing a batch's pixels to the visibility buffer. Only a material with an
/// eval has a vertex shader that writes the varyings the fragment reads. Everything shares
/// one fragment shader but what is cut out, by its visual or its material's alpha.
IGpuPipeline::Ptr resolve_or_compile_visibility(IRenderContext& ctx, const IBatch& batch)
{
    auto material_ptr = batch.material();
    auto shader_source_ptr = batch.shader_source();
    auto* mat = material_ptr ? interface_cast<IMaterial>(material_ptr.get()) : nullptr;
    auto* src = material_ptr ? interface_cast<IShaderSource>(material_ptr.get()) : nullptr;
    string_view eval_src;
    string_view eval_fn;
    string_view vsrc;
    if (mat && src) {
        eval_src = src->get_source(shader_role::kEval);
        eval_fn = src->get_fn_name(shader_role::kEval);
    }
    if (!eval_src.empty()) {
        vsrc = src->get_source(shader_role::kVertex);
    }
    if (vsrc.empty()) {
        vsrc = ext::element_vertex_src;
    }
    const string_view discard_src =
        shader_source_ptr ? shader_source_ptr->get_source(shader_role::kDiscard) : string_view{};
    const float threshold = !eval_src.empty() && !eval_fn.empty() ? mat->get_discard_threshold() : 0.f;

    PipelineOptions po = batch.pipeline_options();
    po.blend_mode = BlendMode::Opaque;
    PipelineContentHasher hasher(kVisibilityContentTag);
    hasher.str(vsrc);
    hasher.str(discard_src);
    hasher.f32(threshold);
    if (threshold > 0.f) {
        hasher.str(eval_src);
        hasher.str(eval_fn);
    }
    hasher.options(po);
    const uint64_t key = hasher.key();

    const auto formats = array_view<const PixelFormat>(kVisibilityFormats, kVisibilityAttachments);
    PipelineCacheKey cache_key{key, kVisibilityFormats[0], DepthFormat::Default, pipeline_target_layout(formats)};
    if (auto pipeline = ctx.pipelines().find(cache_key)) {
        return pipeline;
    }

    string composed;
    if (threshold > 0.f) {
        composed = compose_eval_fragment(visibility_masked_fragment_template, eval_src, eval_fn, threshold);
    } else {
        composed.append(discard_src.empty() ? visibility_fragment_src : visibility_discard_fragment_src);
    }
    if (threshold > 0.f && discard_src.empty()) {
        composed.append(string_view("void velk_visual_discard() {}\n", 30));
    } else {
        composed.append(discard_src);
    }
    return ctx.pipelines().compile_dynamic(string_view(composed), vsrc, key, formats, DepthFormat::Default, po);
}

} // namespace

namespace {

// CPU mirror of the deferred lighting push-constant block
// (`layout(push_constant) PC` in deferred_compute_prelude_src).
VELK_GPU_STRUCT DeferredComputePushC {
    uint32_t globals_base;     // FrameGlobals arena index, GLSL pc.globals_base
    uint32_t surface_light_id; // lighting's light on the surface, for far instances
    float    cam_pos[4];
    uint32_t output_image_id;
    uint32_t albedo_tex_id;
    uint32_t normal_tex_id;
    uint32_t worldpos_tex_id;
    uint32_t material_tex_id;
    uint32_t emissive_tex_id;
    uint32_t width;
    uint32_t height;
    uint32_t light_count;
    uint32_t env_texture_id;
    uint32_t cells_header;     // word of the light cells' header in the culling arena
    uint32_t ltc_matrix_id;    // LTC transform table; 0 = area specular off
    uint32_t lights_base;      // light array index (set = 1 slot 5)
    uint32_t ltc_magnitude_id; // LTC energy / Fresnel table
    float    env_params[2];    // x = intensity, y = rotation_rad (inline)
    uint32_t bounce_hops;      // how many surfaces bounce light passes over: 1 or 2
    uint32_t spare1;
    uint32_t mode;             // what a pass is to do, for passes that do more than one thing
    uint32_t spare3;
    uint32_t spare4;
    uint32_t spare2;
    uint32_t spare5;
};
static_assert(sizeof(DeferredComputePushC) == 128, "Deferred compute PushC layout mismatch");

uint32_t env_uint(const char* name, long lo, long hi)
{
    const auto parse = [&](const char* v) -> uint32_t {
        if (!v || !v[0]) return 0u;
        long n = std::strtol(v, nullptr, 10);
        return (n >= lo && n <= hi) ? static_cast<uint32_t>(n) : 0u;
    };
#ifdef _WIN32
    // _dupenv_s rather than getenv, which MSVC deprecates.
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, name) != 0 || !val) return 0u;
    uint32_t r = parse(val);
    std::free(val);
    return r;
#else
    return parse(std::getenv(name));
#endif
}

// Diagnostic: shows the visibility buffer instead of the image. See visibility_debug_compute_src.
uint32_t visibility_debug_mode() { return env_uint("VELK_VISIBILITY_DEBUG", 1, 6); }

// Diagnostic: the cut-off for clusters drawn in compute, in pixels, in place of the camera's
// tiny_cluster_pixels. 0 when not set.
uint32_t tiny_pixels_override() { return env_uint("VELK_TINY_PIXELS", 1, 64); }

// Diagnostic: shows the light cells under each pixel instead of the image, read each frame so
// that an app can switch it while running. See light_cells_debug_compute_src.
uint32_t light_cells_debug_mode() { return env_uint("VELK_LIGHT_CELLS_DEBUG", 1, 11); }

// Below what roughness a pixel traces its reflection, from VELK_REFLECT_ROUGHNESS in
// hundredths: 0.3 unless set, 0 for none. See light_cells_reflect_trace_main_src.
float light_reflect_roughness()
{
    const char* set = std::getenv("VELK_REFLECT_ROUGHNESS");
    if (!set) return 0.3f;
    return static_cast<float>(std::clamp(std::atoi(set), 0, 100)) / 100.f;
}

uint32_t float_bits(float f)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// How many surfaces bounce light passes over, from VELK_LIGHT_CELL_HOPS: 2 unless set to 1.
// With 1, bounce rays read only the direct light where they hit, and make no bounce cells.
uint32_t light_cell_hops()
{
    const uint32_t hops = env_uint("VELK_LIGHT_CELL_HOPS", 1, 2);
    return hops ? hops : 2u;
}

// How many rays a frame may trace to keep the light cells up, from VELK_LIGHT_CELL_RAYS.
// 262,144 unless set. Cells past that wait for a later frame. A light on a cell is traced
// by a group of 64 threads, a ray each, so this is in steps of 64.
uint32_t light_cell_rays()
{
    const uint32_t rays = env_uint("VELK_LIGHT_CELL_RAYS", 1, 100000000);
    return rays != 0u ? rays : 262144u;
}

// How many pixels across a light cell is on screen, from VELK_LIGHT_CELL_PIXELS. 4 unless set.
float light_cell_pixels()
{
    const uint32_t pixels = env_uint("VELK_LIGHT_CELL_PIXELS", 1, 64);
    return pixels != 0u ? static_cast<float>(pixels) : 4.f;
}

// Render test measurements, while VELK_TEST_DIAGNOSTICS is set.
bool test_diagnostics_enabled() { return std::getenv("VELK_TEST_DIAGNOSTICS") != nullptr; }

// Diagnostic: draws everything the frustum holds, to compare with occlusion culling.
bool no_occlusion_culling() { return env_uint("VELK_NO_OCCLUSION", 1, 1) != 0u; }

// Composes a deferred-family compute pipeline: the shared prelude
// (deferred_compute_prelude_src) + the given main() + the intersect_shape
// and velk_eval_shadow switches built from the frame's snippet set. The
// lighting and sun-visibility shaders are identical except for main(), so
// they compose the same way and differ only by `main_src` + `variant_tag`
// (the tag keeps their weak-cache entries distinct).
IGpuPipeline::Ptr compose_deferred_compute_pipeline(FrameContext& ctx,
                                                    string_view main_src,
                                                    uint64_t variant_tag,
                                                    bool with_area_lights = false,
                                                    bool with_medium = false,
                                                    string_view ray_motion = {},
                                                    float ray_motion_reach = 0.f,
                                                    uint32_t cells = 0)
{
    // `cells`, what of the light cells' code a shader is built with: 1 what reads them; 2 what
    // keeps them up, for the passes that find, make and list cells; 3 what traces them, for the
    // passes that trace rays and nothing else; 4 only the common part.
    if (!ctx.render_ctx || !ctx.snippets) return {};

    const auto& intersect_ids = ctx.snippets->frame_intersects();
    const auto& intersect_info_by_id = ctx.snippets->intersect_info_by_id();
    const auto& shadow_tech_ids = ctx.snippets->frame_shadow_techs();
    const auto& shadow_tech_info = ctx.snippets->shadow_tech_info_by_id();

    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    constexpr uint64_t kDeferredTag = 0x44665232'44666572ULL;
    constexpr uint64_t kShadowTag = 0x53686477'54656368ULL;
    uint64_t key = kFnvBasis ^ kDeferredTag;
    key = (key ^ variant_tag) * kFnvPrime;
    for (auto id : intersect_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key = (key ^ kShadowTag) * kFnvPrime;
    for (auto id : shadow_tech_ids) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    // Distinct variants: the two differ in source, so they must not share a
    // cache entry.
    key = (key ^ (with_area_lights ? 0x41726561ULL : 0x4e6f6e65ULL)) * kFnvPrime;
    key = (key ^ (with_medium ? 0x4d656469ULL : 0x4e6f6e65ULL)) * kFnvPrime;
    key = (key ^ (0x43656c6cULL + cells)) * kFnvPrime;
    for (char c : ray_motion) {
        key = (key ^ static_cast<uint8_t>(c)) * kFnvPrime;
    }
    key |= 0x4000000000000000ULL;

    // The weak pipeline cache is the source of truth: if the pipeline for
    // this snippet combo is still alive (held by a live pass), reuse it;
    // otherwise compose + compile a fresh one.
    if (auto p = ctx.render_ctx->pipelines().find(
            PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0})) {
        return p;
    }

    string src;
    src += deferred_compute_prelude_src;
    // The analytic area-light path is compiled in only for views that have an
    // area light. It never executes elsewhere, but its polygon arrays claim
    // registers in every branch the compiler must allocate for, and the lost
    // occupancy is paid by the whole pass: including it unconditionally cost
    // bistro (which has no area lights) roughly 3x on deferred.lighting.
    src += with_area_lights ? deferred_area_light_src : deferred_area_light_stub_src;
    if (cells == 1) {
        src += light_cells_read_src;
    } else if (cells == 2) {
        src += light_cells_src;
        src += light_cells_touch_src;
    } else if (cells == 3) {
        src += light_cells_src;
        src += light_cells_trace_src;
    } else if (cells == 4) {
        src += light_cells_src;
    }
    src += main_src;
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        src += string_view("#include \"", 10);
        src += ii.include_name;
        src += string_view("\"\n", 2);
    }

    auto append_literal = [&src](const char* s) {
        src += string_view(s, std::strlen(s));
    };

    append_literal("bool intersect_shape(Ray ray, RtShape shape, out RayHit hit) {\n");
    append_literal("    switch (shape.shape_kind) {\n");
    append_literal("        case 1u: return intersect_cube(ray, shape, hit);\n");
    append_literal("        case 2u: return intersect_sphere(ray, shape, hit);\n");
    char buf[128];
    for (auto id : intersect_ids) {
        if (id < 3 || id - 3 >= intersect_info_by_id.size()) continue;
        const auto& ii = intersect_info_by_id[id - 3];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        src += ii.fn_name;
        append_literal("(ray, shape, hit);\n");
    }
    append_literal("        default: return intersect_rect(ray, shape, hit);\n");
    append_literal("    }\n");
    append_literal("}\n");

    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        src += string_view("#include \"", 10);
        src += ti.include_name;
        src += string_view("\"\n", 2);
    }

    append_literal("float velk_eval_shadow(uint tech_id, uint light_idx, vec3 world_pos, vec3 world_normal) {\n");
    append_literal("    switch (tech_id) {\n");
    for (auto id : shadow_tech_ids) {
        if (id == 0 || id > shadow_tech_info.size()) continue;
        const auto& ti = shadow_tech_info[id - 1];
        int n = std::snprintf(buf, sizeof(buf), "        case %uu: return ", id);
        if (n > 0) {
            src += string_view(static_cast<const char*>(buf), static_cast<size_t>(n));
        }
        src += ti.fn_name;
        append_literal("(light_idx, world_pos, world_normal);\n");
    }
    append_literal("        default: return 1.0;\n");
    append_literal("    }\n");
    append_literal("}\n");

    if (!ray_motion.empty()) {
        src = velk_with_ray_motion(string_view(src), ray_motion, ray_motion_reach);
    }
    if (with_medium) {
        const string with_define = velk_with_trace_medium(string_view(src));
        return ctx.render_ctx->pipelines().compile_compute(string_view(with_define), key);
    }
    return ctx.render_ctx->pipelines().compile_compute(string_view(src), key);
}

} // namespace

IGpuPipeline::Ptr DeferredPath::ensure_pipeline(FrameContext& ctx, bool with_area_lights, bool with_medium,
                                                string_view ray_motion, float ray_motion_reach)
{
    return compose_deferred_compute_pipeline(ctx, deferred_lighting_main_src, 0ull,
                                             with_area_lights, with_medium, ray_motion, ray_motion_reach, 1u);
}

namespace {

// The table of light cells: its slots, a power of two, and the words of each. See
// light_cells_main_src.
constexpr uint32_t kCellSlots = 1u << 20;
constexpr uint32_t kCellWords = 32;
// The changes the light cells are given each frame: how many at most, and the words of each
// after 4 words of header. More count as everything changed.
constexpr uint32_t kCellChanges = 128;
constexpr uint32_t kCellChangeWords = 8;
// The words that say where all of that is. See CELLS_HEADER in light_cells_src.
constexpr uint32_t kCellHeaderWords = 17;
// The words of each pixel's reflection ray. See kReflectWords.
constexpr uint32_t kReflectWords = 9;
// The bounce cells' table: its entries, a power of two, and the words of each. See
// VELK_BOUNCE_GLSL.
constexpr uint32_t kBounceEntries = 1u << 15;
constexpr uint32_t kBounceRays = 64;
constexpr uint32_t kBounceWords = 16 + kBounceRays * 5;
// How many bounce cells a frame may trace, and the words of their list: a count and three
// spare, 2 words an entry, then 8 words a ray of each. See CELLS_BOUNCE_LIST.
constexpr uint32_t kBounceListMax = 256;
constexpr uint32_t kBounceListWords = 4 + kBounceListMax * 2 + kBounceListMax * kBounceRays * 8;
// What can be listed for tracing in a frame at most, one light for one cell an entry, the
// words of each, and the header's words before them, counters for the log among them. See
// CELLS_WORK in light_cells_src.
constexpr uint32_t kCellWorkMax = 32768;
constexpr uint32_t kCellWorkWords = 8;
constexpr uint32_t kCellWorkHeader = 40;
constexpr uint32_t kCellStats = 36;
// How often the counters are printed, in frames.
constexpr uint64_t kCellStatFrames = 60;

// How many pixels on screen something that moved must span to wake the cells it could
// shadow, from VELK_LIGHT_CELL_MOVER_PIXELS. A cell's size on screen unless set.
float light_cell_mover_pixels()
{
    const uint32_t pixels = env_uint("VELK_LIGHT_CELL_MOVER_PIXELS", 1, 4096);
    return pixels != 0u ? static_cast<float>(pixels) : light_cell_pixels();
}

// How many entries a frame traces at most, from the rays it may trace.
uint32_t light_cell_entries()
{
    return std::clamp(light_cell_rays() / 64u, 64u, kCellWorkMax);
}

// Mirror the tile grid and layout in test_diagnostics_compute_src. A slot is one frame's words.
constexpr uint32_t kTestTilesX = 16;
constexpr uint32_t kTestTilesY = 9;
constexpr uint32_t kTestWords = 10 + kTestTilesX * kTestTilesY * 2;
constexpr uint32_t kTestSlots = 16;
// How many frames back a slot is read, so that no frame still in flight is writing it.
constexpr uint64_t kTestReadBehind = 8;

// CPU mirror of the RT push-constant block (RtRoot in rt_path.cpp), as the bounce cells'
// material pass uses it.
VELK_GPU_STRUCT BounceShadePushC {
    uint32_t shapes_base;
    uint32_t globals_base;
    uint32_t light_count;
    uint32_t lights_base;
    float    env_params[2];
    uint32_t extras[4];        // x = the bounce list's word, y = how many it may hold
    uint32_t env[4];           // x = env material, y = env texture
};

} // namespace

void DeferredPath::ensure_arena_words(ViewState& vs, FrameContext& ctx)
{
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) return;
    if (test_diagnostics_enabled() && !vs.test_words.valid()) {
        vs.test_words = arena->alloc(uint64_t(kTestWords) * kTestSlots * sizeof(uint32_t));
    }
    if (!vs.light_cells.valid()) {
        vs.light_cell_header = arena->alloc(uint64_t(kCellHeaderWords) * sizeof(uint32_t));
        const uint64_t bytes = uint64_t(kCellSlots) * kCellWords * sizeof(uint32_t);
        vs.light_cells = arena->alloc(bytes);
        if (vs.light_cells.valid()) {
            // Every slot starts empty.
            vector<uint32_t> empty;
            empty.resize(size_t(kCellSlots) * kCellWords);
            arena->write_at(vs.light_cells.offset(), empty.data(), bytes);
        }
        vs.light_cell_change_words = arena->alloc(uint64_t(4u + kCellChanges * kCellChangeWords) * sizeof(uint32_t));
        vs.light_cell_work = arena->alloc(uint64_t(kCellWorkHeader + kCellWorkMax * kCellWorkWords) * sizeof(uint32_t));
        if (vs.light_cell_work.valid()) {
            // The list starts empty, the counters at zero. The trace pass empties the list
            // again each frame.
            const uint32_t none[kCellWorkHeader] = {};
            arena->write_at(vs.light_cell_work.offset(), none, sizeof(none));
        }
        // Every bounce entry starts empty, and so does the list.
        vs.light_bounce = arena->alloc(uint64_t(kBounceEntries) * kBounceWords * sizeof(uint32_t));
        if (vs.light_bounce.valid()) {
            vector<uint32_t> empty;
            empty.resize(size_t(kBounceEntries) * kBounceWords);
            arena->write_at(vs.light_bounce.offset(), empty.data(), empty.size() * sizeof(uint32_t));
        }
        vs.light_bounce_list = arena->alloc(uint64_t(kBounceListWords) * sizeof(uint32_t));
        if (vs.light_bounce_list.valid()) {
            const uint32_t none[4] = {};
            arena->write_at(vs.light_bounce_list.offset(), none, sizeof(none));
        }
    }
    // A reflection ray a pixel, as large as the G-buffer, made again when it grows. The trace
    // writes every pixel each frame, so nothing needs clearing.
    const uint64_t reflect_bytes = uint64_t(vs.gbuffer_size.x) * vs.gbuffer_size.y * kReflectWords * sizeof(uint32_t);
    if (light_reflect_roughness() <= 0.f) {
        vs.light_reflect = {};
    } else if (reflect_bytes > 0 && (!vs.light_reflect.valid() || vs.light_reflect.size() < reflect_bytes)) {
        vs.light_reflect = arena->alloc(reflect_bytes);
    }

    // The list of clusters drawn in compute and the words of each pixel, made again when the
    // frame's size changes. The depth plane compute raster draws to starts clear, and the
    // resolve clears it again after it.
    const uint64_t pixels = uint64_t(vs.gbuffer_size.x) * vs.gbuffer_size.y;
    if (pixels > 0 && (!vs.pixel_words.valid() || vs.pixel_words_size != vs.gbuffer_size)) {
        vs.pixel_words = arena->alloc(pixels * 3u * sizeof(uint32_t));
        vs.pixel_words_size = vs.gbuffer_size;
        if (vs.pixel_words.valid()) {
            vector<uint32_t> clear;
            clear.resize(size_t(pixels));
            arena->write_at(vs.pixel_words.offset(), clear.data(), clear.size() * sizeof(uint32_t));
        }
        vs.tiny_capacity = static_cast<uint32_t>(pixels);
        vs.tiny_list = arena->alloc((uint64_t(kTinyHeaderWords) + pixels * kTinyRecordWords) * sizeof(uint32_t));
        if (vs.tiny_list.valid()) {
            const uint32_t none[kTinyHeaderWords] = {};
            arena->write_at(vs.tiny_list.offset(), none, sizeof(none));
        } else {
            vs.tiny_capacity = 0;
        }
    }
}

uint32_t DeferredPath::pixel_ids_word(const ViewState& vs)
{
    return static_cast<uint32_t>(vs.pixel_words.offset() / sizeof(uint32_t)) + vs.pixel_words_size.x * vs.pixel_words_size.y;
}

void DeferredPath::emit_tiny_cluster_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph, float tiny_pixels)
{
    if (!vs.visibility || !vs.tiny_list.valid() || !vs.pixel_words.valid() || vs.instance_cull.tiny_word == kCullNoSlot) {
        return;
    }
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    IGpuBuffer* arena_buffer = arena ? arena->buffer() : nullptr;
    if (!arena_buffer) return;
    const auto pipeline_for = [&](string_view src, uint64_t key) {
        auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
        return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
    };
    auto args_pipeline = pipeline_for(instance_tiny_args_src, 0x5469'6e79'4172'6773ULL);
    auto draw_pipeline = pipeline_for(instance_tiny_draw_src, 0x5469'6e79'4472'6177ULL);
    if (!args_pipeline || !draw_pipeline) return;

    // How many there were in some recent frame, now and then.
    if (ctx.present_counter >= vs.tiny_log_frame + kCellStatFrames) {
        vs.tiny_log_frame = ctx.present_counter;
        uint32_t header[kTinyHeaderWords];
        arena->read_at(vs.tiny_list.offset(), header, sizeof(header));
        VELK_LOG(I, "clusters drawn in compute: %u, room for %u", header[4], vs.tiny_capacity);
    }

    bool dirty = true;
    emit_cached_view_pass(vs.tiny_pass, dirty, "deferred.tiny", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        const auto list_word = static_cast<uint32_t>(vs.tiny_list.offset() / sizeof(uint32_t));
        cmd->begin_recording();
        cmd->push_label("DeferredPath: compute raster");
        cmd->record_global_barrier();
        {
            const uint32_t pc[2] = {list_word, vs.tiny_capacity};
            DispatchCall dc{};
            dc.pipeline = args_pipeline.get();
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, pc, sizeof(pc));
            cmd->record_dispatch(dc);
        }
        cmd->record_global_barrier();
        // A triangle covers at most a few cut-offs in each direction.
        const auto most = static_cast<uint32_t>(std::max(std::ceil(4.f * tiny_pixels), 1.f));
        for (uint32_t mode = 0; mode < 2; ++mode) {
            struct PushC
            {
                uint32_t list_word, planes_word, width, height, depth_id, mode, most;
            };
            const PushC pc{list_word, static_cast<uint32_t>(vs.pixel_words.offset() / sizeof(uint32_t)), vs.pixel_words_size.x,
                           vs.pixel_words_size.y, vs.visibility->attachment(1), mode, most};
            DispatchCall dc{};
            dc.pipeline = draw_pipeline.get();
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(list_word + 1u) * sizeof(uint32_t));
            cmd->record_global_barrier();
        }
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.held.push_back(std::move(args_pipeline));
        rec.held.push_back(std::move(draw_pipeline));
    });
}

bool DeferredPath::ensure_ltc_tables(FrameContext& ctx, IRenderGraph& graph)
{
    if (ltc_matrix_ && ltc_magnitude_) return true;
    if (ltc_upload_failed_ || !ctx.backend) return false;

    TextureDesc td;
    td.width = ltc::kLtcSize;
    td.height = ltc::kLtcSize;
    td.format = PixelFormat::RGBA32F;
    td.usage = TextureUsage::Sampled;
    // Clamp: the fit is defined only on [0,1]^2, and wrapping would fold
    // grazing angles back onto head-on ones. Linear between texel centres,
    // which is what the generator's (i + 0.5) / N sampling assumes.
    td.sampler.wrap_s = SamplerAddressMode::ClampToEdge;
    td.sampler.wrap_t = SamplerAddressMode::ClampToEdge;
    td.sampler.wrap_r = SamplerAddressMode::ClampToEdge;
    td.sampler.mag_filter = SamplerFilter::Linear;
    td.sampler.min_filter = SamplerFilter::Linear;
    td.sampler.mipmap_mode = SamplerMipmapMode::Nearest;

    auto upload = [&](const float* src, int channels) -> IRenderTarget::Ptr {
        auto rt = graph.resources().create_render_texture(td);
        if (!rt) return {};
        auto* gt = graph.resources().find_texture(rt.get());
        if (!gt) return {};
        // The tables are 4- and 2-channel; the texture is RGBA32F either way,
        // so the 2-channel one is widened here rather than burning a second
        // format on it.
        vector<float> rgba(static_cast<size_t>(ltc::kLtcSize) * ltc::kLtcSize * 4, 0.f);
        for (size_t i = 0; i < static_cast<size_t>(ltc::kLtcSize) * ltc::kLtcSize; ++i) {
            for (int c = 0; c < channels; ++c) rgba[i * 4 + c] = src[i * channels + c];
        }
        ctx.backend->upload_texture(*gt, reinterpret_cast<const uint8_t*>(rgba.data()),
                                    ltc::kLtcSize, ltc::kLtcSize);
        return rt;
    };

    ltc_matrix_ = upload(ltc::kLtcMatrix, 4);
    ltc_magnitude_ = upload(ltc::kLtcMagnitude, 2);
    if (!ltc_matrix_ || !ltc_magnitude_) {
        ltc_matrix_ = {};
        ltc_magnitude_ = {};
        ltc_upload_failed_ = true;  // do not retry every frame
        VELK_LOG(W, "deferred: LTC table upload failed; area lights stay diffuse-only");
        return false;
    }
    return true;
}

IRenderTextureGroup* DeferredPath::ensure_gbuffer(ViewState& vs, int width, int height,
                                                  FrameContext& /*ctx*/,
                                                  IRenderGraph& graph)
{
    if (width <= 0 || height <= 0) return nullptr;

    uvec2 want{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (vs.gbuffer && vs.gbuffer_size == want) {
        return vs.gbuffer.get();
    }

    TextureGroupDesc gdesc{};
    gdesc.formats = array_view<const PixelFormat>(
        kGBufferFormats, static_cast<uint32_t>(GBufferAttachment::Count));
    gdesc.width = width;
    gdesc.height = height;
    gdesc.usage = TextureUsage::Storage;
    vs.gbuffer = graph.resources().create_render_texture_group(gdesc);
    if (!vs.gbuffer) return nullptr;

    vs.gbuffer_size = want;
    vs.lighting_dirty = true;
    vs.transparent_dirty = true;

    if (!vs.worldpos_alias) {
        vs.worldpos_alias = instance().create<IRenderTarget>(ClassId::RenderTexture);
    }
    if (vs.worldpos_alias) {
        auto wp_id = vs.gbuffer->attachment(
            static_cast<uint32_t>(GBufferAttachment::WorldPos));
        vs.worldpos_alias->set_gpu_handle(
            GpuResourceKey::Default, static_cast<uint64_t>(wp_id));
        vs.worldpos_alias->set_size(static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height));
    }
    return vs.gbuffer.get();
}

void DeferredPath::build_passes(IViewEntry& entry,
                                const RenderView& render_view,
                                IRenderTarget::Ptr color_target,
                                FrameContext& ctx,
                                IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.render_ctx) {
        return;
    }
    if (render_view.width <= 0 || render_view.height <= 0) return;

    if (!ctx.backend->supports_primitive_id()) {
        if (!forward_fallback_) {
            forward_fallback_ = instance().create<IRenderPath>(ClassId::Path::Forward);
        }
        if (forward_fallback_) {
            forward_fallback_->build_passes(entry, render_view, color_target, ctx, graph);
        }
        return;
    }

    auto [it, inserted] = view_states_.try_emplace(&entry);
    auto& vs = it->second;
    if (inserted) {
        entry.add_render_state_observer(this);
    }

    auto* gbuffer_handle = ensure_gbuffer(vs, render_view.width, render_view.height, ctx, graph);
    if (!gbuffer_handle) return;

    // Before anything is recorded: growing the culling arena replaces the buffer that
    // recorded commands name.
    ensure_arena_words(vs, ctx);

    // Draws by cluster name the culling arena's buffer, which growing it replaces.
    const uint32_t culling_generation = vs.instance_cull.arena_generation;
    auto* camera = interface_cast<ICamera>(ctx.view_camera_trait);
    auto camera_state = camera ? read_state<ICamera>(camera) : decltype(read_state<ICamera>(camera)){};
    const bool occlusion = (!camera_state || camera_state->occlusion_culling) && vs.depth_pyramid.valid() && !no_occlusion_culling();
    const float tiny_pixels = tiny_pixels_override() != 0u ? static_cast<float>(tiny_pixels_override())
                            : camera_state ? std::max(camera_state->tiny_cluster_pixels, 0.f)
                                           : 2.f;
    vs.instance_cull.tiny_word = vs.tiny_list.valid() && vs.pixel_words.valid() && tiny_pixels > 0.f
        ? static_cast<uint32_t>(vs.tiny_list.offset() / sizeof(uint32_t))
        : kCullNoSlot;
    vs.instance_cull.tiny_capacity = vs.tiny_capacity;
    vs.instance_cull.tiny_pixels = tiny_pixels;
    emit_instance_cull_pass(vs.instance_cull, render_view.batches, render_view.view_globals_base, ctx, graph, true,
                            occlusion ? static_cast<uint32_t>(vs.depth_pyramid.offset() / sizeof(uint32_t)) : kCullNoSlot);
    if (vs.instance_cull.arena_generation != culling_generation) {
        vs.visibility_dirty = true;
        vs.transparent_dirty = true;
    }
    emit_visibility_pass(vs, render_view, ctx, graph, false);
    emit_depth_pyramid_pass(vs, ctx, graph);
    if (occlusion && vs.instance_cull.pyramid_word != kCullNoSlot) {
        emit_instance_retest_pass(vs.instance_cull, render_view.batches, render_view.view_globals_base, ctx, graph);
        emit_visibility_pass(vs, render_view, ctx, graph, true);
    }
    emit_tiny_cluster_pass(vs, ctx, graph, tiny_pixels);
    emit_visibility_resolve_pass(vs, render_view, ctx, graph);
    const uint32_t visibility_mode = visibility_debug_mode();

    if (vs.gbuffer_size.x == 0 || vs.gbuffer_size.y == 0) return;
    if (prepare_light_cells(vs, render_view, ctx)) {
        emit_light_cells_update_pass(vs, render_view, ctx, graph);
    }
    emit_lighting_pass(entry, vs, render_view, color_target, ctx,
                       static_cast<int>(vs.gbuffer_size.x),
                       static_cast<int>(vs.gbuffer_size.y), graph);
    emit_far_instance_pass(vs, render_view, color_target, ctx, static_cast<int>(vs.gbuffer_size.x),
                           static_cast<int>(vs.gbuffer_size.y), graph);
    if (test_diagnostics_enabled()) {
        emit_test_diagnostics_pass(vs, render_view, color_target, ctx, graph);
    }
    if (visibility_mode != 0) {
        emit_visibility_debug_pass(vs, render_view, color_target, ctx, visibility_mode, graph);
    }
    const uint32_t cells_mode = light_cells_debug_mode();
    if (cells_mode >= 4) {
        emit_light_cells_pass(vs, render_view, color_target, ctx, cells_mode, graph);
    } else if (cells_mode != 0) {
        emit_light_cells_debug_pass(vs, render_view, color_target, ctx, cells_mode, graph);
    }
    // Blended geometry draws forward, over the lit composite, against the
    // retained gbuffer depth.
    if (visibility_mode == 0 && cells_mode == 0) {
        emit_transparent_pass(entry, vs, render_view, color_target, ctx, graph);
    }
}

void DeferredPath::emit_visibility_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx,
                                        IRenderGraph& graph, bool second_phase)
{
    if (!render_view.batches || vs.gbuffer_size.x == 0 || vs.gbuffer_size.y == 0) return;

    if (!vs.visibility || vs.visibility_size != vs.gbuffer_size) {
        TextureGroupDesc desc{};
        desc.formats = array_view<const PixelFormat>(kVisibilityFormats, kVisibilityAttachments);
        desc.width = static_cast<int>(vs.gbuffer_size.x);
        desc.height = static_cast<int>(vs.gbuffer_size.y);
        desc.depth = DepthFormat::Default;
        vs.visibility = graph.resources().create_render_texture_group(desc);
        if (!vs.visibility) return;
        vs.visibility_size = vs.gbuffer_size;
        vs.visibility_dirty = true;
    }

    if (!second_phase && vs.visibility_dirty) {
        vs.visibility_second_dirty = true;
    }
    emit_cached_view_pass(
        second_phase ? vs.cached_visibility_second_pass : vs.cached_visibility_pass,
        second_phase ? vs.visibility_second_dirty : vs.visibility_dirty,
        second_phase ? "deferred.visibility2" : "deferred.visibility", graph, [&](CachedPassRecording& rec) {
            auto* default_uv1 = ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
            auto resolve = [&](const IBatch& b) -> IGpuPipeline* {
                if (b.pipeline_options().blend_mode == BlendMode::Alpha) {
                    return nullptr;
                }
                auto p = resolve_or_compile_visibility(*ctx.render_ctx, b);
                if (!p) return nullptr;
                IGpuPipeline* raw = p.get();
                rec.held.push_back(std::move(p));
                return raw;
            };
            vector<DrawCall> draw_calls;
            const FrameResolveContext resolve_ctx = ctx.make_resolve_context();
            emit_draw_calls(draw_calls, *render_view.batches, *ctx.frame_buffer, *ctx.resources, default_uv1,
                            render_view.view_globals_base, resolve,
                            render_view.has_frustum ? &render_view.frustum : nullptr, ctx.snippets, &resolve_ctx, second_phase);

            auto cmd = ctx.backend->create_command_buffer();
            if (!cmd) return;
            ColorAttachment colors[kVisibilityAttachments]{};
            for (uint32_t i = 0; i < kVisibilityAttachments; ++i) {
                colors[i].texture = vs.visibility->attachment_texture(i);
                colors[i].clear = !second_phase;
            }
            DepthAttachment depth{};
            depth.texture = vs.visibility->depth_attachment();
            depth.clear = !second_phase;
            depth.clear_stencil = 0;

            cmd->begin_recording();
            cmd->push_label(second_phase ? "DeferredPath: visibility, second phase" : "DeferredPath: visibility");
            cmd->record_begin_rendering(array_view<const ColorAttachment>(colors, kVisibilityAttachments),
                                        depth.texture ? &depth : nullptr);
            cmd->set_viewport(rect{0, 0, static_cast<float>(vs.gbuffer_size.x), static_cast<float>(vs.gbuffer_size.y)});
            cmd->record_draws({draw_calls.data(), draw_calls.size()});
            cmd->record_end_rendering();
            cmd->pop_label();
            cmd->end_recording();
            rec.cmd = std::move(cmd);
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        });
}

void DeferredPath::emit_depth_pyramid_pass(ViewState& vs, FrameContext& ctx, IRenderGraph& graph)
{
    if (!vs.visibility) return;
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) return;

    if (vs.depth_pyramid_size != vs.visibility_size || !vs.depth_pyramid.valid()) {
        // Starting from half the frame, down to a single texel.
        uint32_t words = kDepthPyramidHeaderWords;
        uint32_t width = std::max(vs.visibility_size.x / 2u, 1u);
        uint32_t height = std::max(vs.visibility_size.y / 2u, 1u);
        for (uint32_t level = 0; level < kDepthPyramidMaxLevels; ++level) {
            words += std::max(width >> level, 1u) * std::max(height >> level, 1u);
            if ((width >> level) <= 1u && (height >> level) <= 1u) break;
        }
        vs.depth_pyramid = arena->alloc(uint64_t(words) * sizeof(uint32_t));
        if (!vs.depth_pyramid.valid()) return;
        vs.depth_pyramid_size = vs.visibility_size;

        uint32_t header[kDepthPyramidHeaderWords]{};
        header[1] = width;
        header[2] = height;
        uint32_t at = static_cast<uint32_t>(vs.depth_pyramid.offset() / sizeof(uint32_t)) + kDepthPyramidHeaderWords;
        uint32_t levels = 0;
        for (; levels < kDepthPyramidMaxLevels; ++levels) {
            header[3 + levels] = at;
            at += std::max(width >> levels, 1u) * std::max(height >> levels, 1u);
            if ((width >> levels) <= 1u && (height >> levels) <= 1u) {
                ++levels;
                break;
            }
        }
        // Left at no levels until the first one has been built, so nothing reads it empty.
        vs.depth_pyramid_levels = levels;
        arena->write_at(vs.depth_pyramid.offset(), header, sizeof(header));
    }

    constexpr uint64_t kKey = 0x4450'5952'414d'0001ULL;
    auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{kKey, PixelFormat::RGBA8, DepthFormat::None, 0});
    auto pipeline = found ? found : ctx.render_ctx->pipelines().compile_compute(depth_pyramid_compute_src, kKey);
    if (!pipeline) return;

    bool dirty = true;
    emit_cached_view_pass(vs.depth_pyramid_pass, dirty, "deferred.pyramid", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        struct PushC
        {
            uint32_t depth_id, source_word, source_width, source_height, target_word, width, height;
        };
        const uint32_t base = static_cast<uint32_t>(vs.depth_pyramid.offset() / sizeof(uint32_t));
        const uint32_t first_width = std::max(vs.visibility_size.x / 2u, 1u);
        const uint32_t first_height = std::max(vs.visibility_size.y / 2u, 1u);
        uint32_t words[kDepthPyramidHeaderWords];
        arena->read_at(vs.depth_pyramid.offset(), words, sizeof(words));

        cmd->begin_recording();
        cmd->push_label("DeferredPath: depth pyramid");
        for (uint32_t level = 0; level < vs.depth_pyramid_levels; ++level) {
            const uint32_t width = std::max(first_width >> level, 1u);
            const uint32_t height = std::max(first_height >> level, 1u);
            const PushC pc{vs.visibility->attachment(1),
                           level == 0 ? 0xFFFFFFFFu : words[3 + level - 1],
                           level == 0 ? vs.visibility_size.x : std::max(first_width >> (level - 1), 1u),
                           level == 0 ? vs.visibility_size.y : std::max(first_height >> (level - 1), 1u),
                           words[3 + level], width, height};
            DispatchCall dc{};
            dc.pipeline = pipeline.get();
            dc.groups_x = (width + 7u) / 8u;
            dc.groups_y = (height + 7u) / 8u;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            cmd->record_dispatch(dc);
            cmd->record_global_barrier();
        }
        const uint32_t levels = vs.depth_pyramid_levels;
        cmd->pop_label();
        cmd->end_recording();
        arena->update_at(uint64_t(base) * sizeof(uint32_t), &levels, sizeof(levels));
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.held.push_back(std::move(pipeline));
    });
}

void DeferredPath::emit_visibility_resolve_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx,
                                                IRenderGraph& graph)
{
    if (!vs.visibility || !render_view.batches || !ctx.snippets || !vs.pixel_words.valid()) return;

    // The frame's list of materials decides which evals the shader composes.
    const FrameResolveContext resolve_ctx = ctx.make_resolve_context();
    for (const auto& batch : *render_view.batches) {
        if (batch && batch->material() && batch->pipeline_options().blend_mode != BlendMode::Alpha) {
            ctx.snippets->resolve_material(batch->material().get(), resolve_ctx);
        }
    }

    constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
    uint64_t key = kFnvBasis ^ 0x5649'53526573ULL;
    for (auto id : ctx.snippets->frame_materials()) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    for (auto id : ctx.snippets->frame_shadow_techs()) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    for (auto id : ctx.snippets->frame_intersects()) {
        key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
    }
    key |= 0x1000000000000000ULL;
    auto pipeline = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
    if (!pipeline) {
        pipeline = ctx.render_ctx->pipelines().compile_compute(
            string_view(compose_rt_compute(*ctx.snippets, visibility_resolve_main_src)), key);
    }
    if (!pipeline) return;

    const auto handle = [&](GBufferAttachment a) { return vs.gbuffer->attachment(static_cast<uint32_t>(a)); };
    struct PushC
    {
        uint32_t shapes_base, globals_base, light_count, lights_base;
        float env_params[2];
        uint32_t extras[4];
        uint32_t env[4];
    };
    // No lights here, so their fields carry the surface attachment and the words of each
    // pixel. See RESOLVE_SURFACE and RESOLVE_TINY.
    const PushC pc{0u, render_view.view_globals_base, static_cast<uint32_t>(vs.pixel_words.offset() / sizeof(uint32_t)),
                   handle(GBufferAttachment::Surface), {0.f, 0.f},
                   {vs.visibility->attachment(0), vs.visibility_size.x, vs.visibility_size.y, handle(GBufferAttachment::Albedo)},
                   {handle(GBufferAttachment::Normal), handle(GBufferAttachment::WorldPos),
                    handle(GBufferAttachment::MaterialParams), handle(GBufferAttachment::Emissive)}};

    bool dirty = true;
    emit_cached_view_pass(vs.visibility_resolve_pass, dirty, "deferred.resolve", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (vs.visibility_size.x + 7u) / 8u;
        dc.groups_y = (vs.visibility_size.y + 7u) / 8u;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        cmd->begin_recording();
        cmd->push_label("DeferredPath: visibility resolve");
        // Compute raster's words before, and the pixels' ids for the passes after.
        cmd->record_global_barrier();
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        rec.held.push_back(std::move(pipeline));
    });
}

void DeferredPath::emit_visibility_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                              FrameContext& ctx, uint32_t mode, IRenderGraph& graph)
{
    if (!vs.visibility || !vs.deferred_output) return;

    constexpr uint64_t kKey = 0x5649'5344'4247'0001ULL;
    auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{kKey, PixelFormat::RGBA8, DepthFormat::None, 0});
    auto pipeline = found ? found : ctx.render_ctx->pipelines().compile_compute(visibility_debug_compute_src, kKey);
    if (!pipeline) return;

    IGpuTexture* src_tex = graph.resources().find_texture(vs.deferred_output.get());
    if (!src_tex) src_tex = vs.deferred_output_tex;
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) dst_tex = graph.resources().find_texture(color_target.get());
        if (!dst_tex) dst_tex = ctx.resources->find_texture(color_target.get());
    }

    bool dirty = true;
    emit_cached_view_pass(vs.visibility_debug_pass, dirty, "deferred.visibility_debug", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        struct PushC
        {
            uint32_t ids_id, output_id, width, height, mode, pyramid_word;
        };
        const PushC pc{pixel_ids_word(vs),
                       static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default)),
                       vs.visibility_size.x, vs.visibility_size.y, mode,
                       static_cast<uint32_t>(vs.depth_pyramid.offset() / sizeof(uint32_t))};
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (vs.visibility_size.x + 7u) / 8u;
        dc.groups_y = (vs.visibility_size.y + 7u) / 8u;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));

        cmd->begin_recording();
        cmd->push_label("DeferredPath: visibility debug");
        cmd->record_global_barrier();
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();
        if (src_tex && dst_tex) {
            cmd->record_blit_to_texture(*src_tex, *dst_tex, render_view.viewport);
        }
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
        if (color_target) {
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        }
        rec.held.push_back(pipeline);
    });
}

void DeferredPath::emit_test_diagnostics_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                              FrameContext& ctx, IRenderGraph& graph)
{
    if (!vs.test_words.valid() || !vs.gbuffer || !vs.deferred_output) return;
    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) return;

    if (ctx.present_counter >= kTestReadBehind) {
        const uint64_t read_frame = ctx.present_counter - kTestReadBehind;
        uint32_t w[kTestWords];
        arena->read_at(vs.test_words.offset() + (read_frame % kTestSlots) * kTestWords * sizeof(uint32_t), w, sizeof(w));
        constexpr float kScale = 256.f;
        const auto ratio = [](uint32_t part, uint32_t whole) { return whole != 0u ? static_cast<float>(part) / static_cast<float>(whole) : 0.f; };
        float tile_max = 0.f;
        for (uint32_t t = 0; t < kTestTilesX * kTestTilesY; ++t) {
            const uint32_t tile_valid = w[10 + t];
            if (tile_valid >= 64u) {
                tile_max = std::max(tile_max, ratio(w[10 + kTestTilesX * kTestTilesY + t], tile_valid) / kScale);
            }
        }
        // The largest connected group of tiles whose light changed, so that a patch changing
        // together cannot hide in an average over the screen.
        constexpr float kPatchChange = 0.01f;
        constexpr uint32_t kTiles = kTestTilesX * kTestTilesY;
        bool changed[kTiles];
        for (uint32_t t = 0; t < kTiles; ++t) {
            changed[t] = w[10 + t] >= 64u && ratio(w[10 + kTiles + t], w[10 + t]) / kScale > kPatchChange;
        }
        uint32_t patch = 0;
        uint32_t stack[kTiles];
        for (uint32_t t = 0; t < kTiles; ++t) {
            if (!changed[t]) continue;
            changed[t] = false;
            uint32_t depth = 0;
            uint32_t tiles = 0;
            stack[depth++] = t;
            while (depth > 0) {
                const uint32_t at = stack[--depth];
                ++tiles;
                const auto visit = [&](uint32_t next) {
                    if (changed[next]) {
                        changed[next] = false;
                        stack[depth++] = next;
                    }
                };
                if (at % kTestTilesX != 0u) visit(at - 1u);
                if (at % kTestTilesX != kTestTilesX - 1u) visit(at + 1u);
                if (at >= kTestTilesX) visit(at - kTestTilesX);
                if (at + kTestTilesX < kTiles) visit(at + kTestTilesX);
            }
            patch = std::max(patch, tiles);
        }
        VELK_LOG(I, "test_frame frame=%llu valid=%u mean=%.5f over5=%.5f over20=%.5f up_valid=%u up_mean=%.5f up_over5=%.5f up_over20=%.5f tile_max=%.5f tested=%u missing=%u patch=%u",
                 static_cast<unsigned long long>(read_frame), w[0], ratio(w[1], w[0]) / kScale, ratio(w[2], w[0]), ratio(w[3], w[0]), w[4],
                 ratio(w[5], w[4]) / kScale, ratio(w[6], w[4]), ratio(w[7], w[4]), tile_max, w[8], w[9], patch);
    }

    constexpr uint64_t kKey = 0x5445'5354'4449'0001ULL;
    auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{kKey, PixelFormat::RGBA8, DepthFormat::None, 0});
    auto pipeline = found ? found : ctx.render_ctx->pipelines().compile_compute(test_diagnostics_compute_src, kKey);
    if (!pipeline) return;

    const auto width = vs.gbuffer_size.x;
    const auto height = vs.gbuffer_size.y;
    if (!vs.test_history[0] || vs.test_history_size != vs.gbuffer_size) {
        TextureDesc td{};
        td.width = static_cast<int>(width);
        td.height = static_cast<int>(height);
        td.format = PixelFormat::RGBA32F;
        td.usage = TextureUsage::Storage;
        td.sampler.wrap_s = SamplerAddressMode::ClampToEdge;
        td.sampler.wrap_t = SamplerAddressMode::ClampToEdge;
        td.sampler.mag_filter = SamplerFilter::Nearest;
        td.sampler.min_filter = SamplerFilter::Nearest;
        td.sampler.mipmap_mode = SamplerMipmapMode::Nearest;
        vs.test_history[0] = graph.resources().create_render_texture(td);
        vs.test_history[1] = graph.resources().create_render_texture(td);
        vs.test_history_size = vs.gbuffer_size;
    }
    if (!vs.test_history[0] || !vs.test_history[1]) return;

    // Shows what the flicker measure sees instead of the image.
    const bool show_flicker = std::getenv("VELK_TEST_FLICKER_VIEW") != nullptr;
    if (show_flicker && (!vs.test_view || vs.test_view_size != vs.gbuffer_size)) {
        TextureDesc td{};
        td.width = static_cast<int>(width);
        td.height = static_cast<int>(height);
        td.format = PixelFormat::RGBA16F;
        td.usage = TextureUsage::Storage;
        vs.test_view = graph.resources().create_render_texture(td);
        vs.test_view_size = vs.gbuffer_size;
    }
    IGpuTexture* view_tex = show_flicker && vs.test_view ? graph.resources().find_texture(vs.test_view.get()) : nullptr;
    IGpuTexture* view_dst = nullptr;
    if (view_tex && color_target) {
        view_dst = interface_cast<IGpuTexture>(color_target.get());
        if (!view_dst) view_dst = graph.resources().find_texture(color_target.get());
        if (!view_dst) view_dst = ctx.resources->find_texture(color_target.get());
    }

    const auto handle = [](const IRenderTarget::Ptr& t) { return static_cast<uint32_t>(t->get_gpu_handle(GpuResourceKey::Default)); };
    const uint32_t parity = static_cast<uint32_t>(ctx.present_counter & 1ull);
    auto previous = vs.test_history[parity];
    auto current = vs.test_history[1u - parity];
    const uint32_t first_word = static_cast<uint32_t>(vs.test_words.offset() / sizeof(uint32_t))
                                + static_cast<uint32_t>(ctx.present_counter % kTestSlots) * kTestWords;

    bool dirty = true;
    emit_cached_view_pass(vs.test_diagnostics_pass, dirty, "test.diagnostics", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        VELK_GPU_STRUCT PushC
        {
            uint32_t word, words, mode, globals_base;
            uint32_t normal_id, pos_id, albedo_id, color_id, history_id, output_id, width, height, tiles_up_only, view_id;
        };
        PushC pc{};
        pc.word = first_word;
        pc.words = kTestWords;
        pc.globals_base = render_view.view_globals_base;
        pc.normal_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
        pc.pos_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
        pc.albedo_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Albedo));
        // What the flicker measure follows: the finished image unless told otherwise.
        const char* source = std::getenv("VELK_TEST_FLICKER_SOURCE");
        const auto from = [&](const char* name) { return source && std::strcmp(source, name) == 0; };
        const uint32_t measure_mode = from("albedo") ? 5u : 1u;
        pc.color_id = handle(vs.deferred_output);
        pc.history_id = handle(previous);
        pc.output_id = handle(current);
        pc.width = width;
        pc.height = height;
        pc.tiles_up_only = std::getenv("VELK_TEST_TILES_UP") ? 1u : 0u;
        pc.view_id = view_tex ? handle(vs.test_view) : 0u;

        DispatchCall clear{};
        clear.pipeline = pipeline.get();
        clear.groups_x = (kTestWords + 63u) / 64u;
        clear.groups_y = 1;
        clear.groups_z = 1;
        clear.root_constants_size = sizeof(pc);
        std::memcpy(clear.root_constants, &pc, sizeof(pc));

        pc.mode = measure_mode;
        DispatchCall measure{};
        measure.pipeline = pipeline.get();
        measure.groups_x = (width + 7u) / 8u;
        measure.groups_y = (height + 7u) / 8u;
        measure.groups_z = 1;
        measure.root_constants_size = sizeof(pc);
        std::memcpy(measure.root_constants, &pc, sizeof(pc));

        cmd->begin_recording();
        cmd->record_global_barrier();
        cmd->record_dispatch(clear);
        cmd->record_global_barrier();
        cmd->record_dispatch(measure);
        cmd->record_global_barrier();
        if (view_tex && view_dst) {
            cmd->record_blit_to_texture(*view_tex, *view_dst, render_view.viewport);
        }
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(previous));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(current));
        rec.held.push_back(pipeline);
    });
}

void DeferredPath::emit_light_cells_debug_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                               FrameContext& ctx, uint32_t mode, IRenderGraph& graph)
{
    if (!vs.visibility || !vs.gbuffer || !vs.deferred_output) return;

    constexpr uint64_t kKey = 0x4c43'454c'4c44'0001ULL;
    auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{kKey, PixelFormat::RGBA8, DepthFormat::None, 0});
    auto pipeline = found ? found : ctx.render_ctx->pipelines().compile_compute(light_cells_debug_compute_src, kKey);
    if (!pipeline) return;

    IGpuTexture* src_tex = graph.resources().find_texture(vs.deferred_output.get());
    if (!src_tex) src_tex = vs.deferred_output_tex;
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) dst_tex = graph.resources().find_texture(color_target.get());
        if (!dst_tex) dst_tex = ctx.resources->find_texture(color_target.get());
    }

    bool dirty = true;
    emit_cached_view_pass(vs.light_cells_debug_pass, dirty, "light_cells.debug", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        VELK_GPU_STRUCT PushC
        {
            uint32_t ids_word, surface_id, pos_id, normal_id, output_id, width, height, mode, globals_base;
            float pixels;
            uint32_t objects, object_slots;
        };
        const auto attachment = [&](GBufferAttachment a) { return vs.gbuffer->attachment(static_cast<uint32_t>(a)); };
        const PushC pc{pixel_ids_word(vs),
                       attachment(GBufferAttachment::Surface),
                       attachment(GBufferAttachment::WorldPos),
                       attachment(GBufferAttachment::Normal),
                       static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default)),
                       vs.gbuffer_size.x,
                       vs.gbuffer_size.y,
                       mode,
                       render_view.view_globals_base,
                       light_cell_pixels(),
                       render_view.bvh.object_transforms,
                       render_view.bvh.object_slots};
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (vs.gbuffer_size.x + 7u) / 8u;
        dc.groups_y = (vs.gbuffer_size.y + 7u) / 8u;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));

        cmd->begin_recording();
        cmd->push_label("DeferredPath: light cells debug");
        cmd->record_global_barrier();
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();
        if (src_tex && dst_tex) {
            cmd->record_blit_to_texture(*src_tex, *dst_tex, render_view.viewport);
        }
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
        if (color_target) {
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        }
        rec.held.push_back(pipeline);
    });
}

bool DeferredPath::prepare_light_cells(ViewState& vs, const RenderView& render_view, FrameContext& ctx)
{
    if (!vs.visibility || !vs.gbuffer || !ctx.resources) return false;

    // Allocated in ensure_arena_words, before anything is recorded.
    if (!vs.light_cells.valid() || !vs.light_cell_change_words.valid() || !vs.light_cell_header.valid()
        || !vs.light_cell_work.valid() || !vs.light_bounce.valid() || !vs.light_bounce_list.valid()) {
        return false;
    }


    // The counters, per frame since last printed. They are read while frames are on the
    // GPU, so a frame's worth may land on either side; over a second that does not matter.
    if (auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
        arena && ctx.present_counter >= vs.light_cell_stats_frame + kCellStatFrames) {
        uint32_t now[kCellStats];
        arena->read_at(vs.light_cell_work.offset() + 4u * sizeof(uint32_t), now, sizeof(now));
        uint32_t d[kCellStats];
        for (uint32_t i = 0; i < kCellStats; ++i) d[i] = now[i] - vs.light_cell_stats[i];
        const float frames = static_cast<float>(std::max(d[7], 1u));
        const auto per = [&](uint32_t i) { return static_cast<double>(static_cast<float>(d[i]) / frames); };
        if (vs.light_cell_stats_frame != 0) {
            VELK_LOG(I,
                     "light cells per frame: entries asked %.0f listed %.0f traced %.0f, cells listed %.0f made %.0f "
                     "changed %.0f (all lights %.0f) refused %.0f busy %.0f, pixels waiting %.0f no slot %.0f, changes %u; "
                     "coarse round entries asked %.0f cells refused %.0f, waiting pixels without coarse %.0f; "
                     "moving in the way: lights held %.0f, cells re-traced %.0f; "
                     "bounce cells asking new %.0f stale %.0f, listed %.0f, gathered %.0f, with nothing to read %.0f, "
                     "rays with nothing to read %.0f, rays that hit their own object's back %.0f; "
                     "bounce cells made by rays %.0f, refused %.0f, listed %.0f, gathered %.0f",
                     per(0), per(1), per(8), per(2), per(10), per(9), per(11), per(3), per(4), per(6), per(5),
                     static_cast<unsigned>(vs.light_cell_changes.size()), per(12), per(13), per(14), per(16), per(15),
                     per(17), per(18), per(19), per(20), per(21), per(22), per(23), per(24), per(25), per(26), per(27));
            const double listed_frames = static_cast<double>(ctx.present_counter - vs.light_cell_stats_frame);
            VELK_LOG(I,
                     "light cell changes per frame: boxes listed %.1f, too small %.1f, lights moved %.2f; "
                     "lights on cells found changed by a box to a sun %.0f, to an area light %.0f, to a point or "
                     "spot %.0f, by the light moving %.0f; of all those, inside the box %.0f, above a pixel's own level %.0f; "
                     "cells marked %.0f, could not be placed %.0f",
                     vs.light_cell_boxes_listed / listed_frames, vs.light_cell_boxes_small / listed_frames,
                     vs.light_cell_lights_moved / listed_frames, per(28), per(29), per(30), per(31), per(32), per(33),
                     per(34), per(35));
        }
        for (uint32_t i = 0; i < kCellStats; ++i) vs.light_cell_stats[i] = now[i];
        vs.light_cell_stats_frame = ctx.present_counter;
        vs.light_cell_boxes_listed = 0;
        vs.light_cell_boxes_small = 0;
        vs.light_cell_lights_moved = 0;
    }

    // What changed this frame, which the mark passes put on the cells and bounce entries it
    // could alter: what the scene's rays can hit, and lights that moved. A light's colour and
    // strength are not part of what a cell keeps, so those changing needs nothing traced.
    // Nothing is kept from frame to frame: the marks are.
    {
        const auto now = static_cast<uint32_t>(ctx.present_counter);
        auto& changes = vs.light_cell_changes;
        changes.clear();
        bool all_changed = false;
        const auto forget_all = [&] {
            changes.clear();
            all_changed = true;
        };
        if (render_view.bvh.changed_all) {
            forget_all();
        } else {
            // Something that changed and is smaller on screen than light_cell_mover_pixels
            // wakes no cell: what its shadow could change is no larger. How wide a pixel is,
            // per unit of distance from the camera.
            const mat4& vp = render_view.view_projection;
            const float row = std::sqrt(vp(1, 0) * vp(1, 0) + vp(1, 1) * vp(1, 1) + vp(1, 2) * vp(1, 2));
            const float pixel = row > 0.f && render_view.height > 0 ? 2.f / (row * static_cast<float>(render_view.height)) : 0.f;
            const float least = light_cell_mover_pixels() * pixel;
            const float cam[3] = {render_view.cam_pos.x, render_view.cam_pos.y, render_view.cam_pos.z};
            for (const auto& box : render_view.bvh.changed) {
                float extent = 0.f;
                float distance = 0.f;
                for (int k = 0; k < 3; ++k) {
                    extent = std::max(extent, box.max[k] - box.min[k]);
                    const float c = cam[k];
                    const float out = std::max(std::max(box.min[k] - c, c - box.max[k]), 0.f);
                    distance += out * out;
                }
                if (extent < least * std::sqrt(distance)) {
                    ++vs.light_cell_boxes_small;
                    continue;
                }
                ++vs.light_cell_boxes_listed;
                changes.push_back({box, now, 0xFFFFFFFFu});
            }
        }
        // Lights are compared with how each was when last listed, not with last frame, so
        // that a light drifting a little each frame is listed once the drift adds up to
        // something a shadow could show: a quarter of its size, or for one without a size
        // a five-hundredth of its range.
        const auto& lights = render_view.lights;
        auto& listed = vs.light_cell_lights;
        bool all_lights = lights.size() != listed.size();
        for (size_t i = 0; !all_lights && i < lights.size(); ++i) {
            const GpuLight& a = listed[i];
            const GpuLight& b = lights[i];
            const bool same_kind = a.flags[0] == b.flags[0] && std::memcmp(a.direction, b.direction, sizeof(a.direction)) == 0
                && std::memcmp(a.params, b.params, sizeof(a.params)) == 0;
            float moved = 0.f;
            for (int k = 0; k < 3; ++k) {
                moved += (a.position[k] - b.position[k]) * (a.position[k] - b.position[k]);
            }
            const float least = std::max(0.25f * b.params[3], 0.002f * b.params[0]);
            if (same_kind && moved <= least * least) {
                continue;
            }
            // A light without a place reaches everywhere.
            if (a.flags[0] == static_cast<uint32_t>(LightType::Directional)
                || b.flags[0] == static_cast<uint32_t>(LightType::Directional)) {
                all_lights = true;
                break;
            }
            // The light's reach is checked against where it is now.
            WorldBox box;
            for (int k = 0; k < 3; ++k) {
                box.min[k] = b.position[k];
                box.max[k] = b.position[k];
            }
            changes.push_back({box, now, static_cast<uint32_t>(i)});
            ++vs.light_cell_lights_moved;
            listed[i] = b;
        }
        if (all_lights) {
            forget_all();
            listed.resize(lights.size());
            if (!lights.empty()) {
                std::memcpy(listed.data(), lights.data(), lights.size() * sizeof(GpuLight));
            }
        }

        // More than the list holds: everything counts as changed.
        if (changes.size() > kCellChanges) {
            forget_all();
        }

        vector<uint32_t> words;
        words.resize(4u + kCellChanges * kCellChangeWords);
        words[0] = static_cast<uint32_t>(changes.size());
        words[1] = all_changed ? 1u : 0u;
        words[2] = all_changed || !changes.empty() ? 1u : 0u;
        for (size_t i = 0; i < changes.size(); ++i) {
            uint32_t* w = words.data() + 4u + i * kCellChangeWords;
            std::memcpy(w, changes[i].box.min, sizeof(float) * 3);
            w[3] = changes[i].frame;
            std::memcpy(w + 4, changes[i].box.max, sizeof(float) * 3);
            w[7] = changes[i].light;
        }
        auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
        if (!arena) return false;
        arena->update_at(vs.light_cell_change_words.offset(), words.data(), words.size() * sizeof(uint32_t));

        // Where everything is, for the shaders. See CELLS_HEADER in light_cells_src.
        const uint32_t header[kCellHeaderWords] = {
            static_cast<uint32_t>(vs.light_cells.offset() / sizeof(uint32_t)),
            kCellSlots,
            static_cast<uint32_t>(light_cell_pixels()),
            static_cast<uint32_t>(vs.light_cell_change_words.offset() / sizeof(uint32_t)),
            pixel_ids_word(vs),
            vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Surface)),
            static_cast<uint32_t>(vs.light_cell_work.offset() / sizeof(uint32_t)),
            light_cell_entries(),
            static_cast<uint32_t>(vs.light_bounce.offset() / sizeof(uint32_t)),
            kBounceEntries,
            static_cast<uint32_t>(vs.light_bounce_list.offset() / sizeof(uint32_t)),
            kBounceListMax,
            render_view.bvh.object_transforms,
            render_view.bvh.object_slots,
            vs.light_reflect.valid() ? static_cast<uint32_t>(vs.light_reflect.offset() / sizeof(uint32_t)) : 0xFFFFFFFFu,
            vs.gbuffer_size.x,
            float_bits(light_reflect_roughness()),
        };
        arena->update_at(vs.light_cell_header.offset(), header, sizeof(header));
    }
    return true;
}

void DeferredPath::emit_light_cells_update_pass(ViewState& vs, const RenderView& render_view, FrameContext& ctx,
                                                IRenderGraph& graph)
{
    if (!vs.gbuffer || !vs.light_cell_header.valid()) return;

    const bool has_medium = render_view.bvh.has_medium != 0u;
    auto pipeline = compose_deferred_compute_pipeline(ctx, light_cells_update_main_src, 0x4c43'5550'4454ULL, false, has_medium,
                                                      render_view.bvh.ray_motion, render_view.bvh.ray_motion_reach, 2u);
    auto trace_pipeline = compose_deferred_compute_pipeline(ctx, light_cells_trace_main_src, 0x4c43'5452'4143ULL, false, has_medium,
                                                            render_view.bvh.ray_motion, render_view.bvh.ray_motion_reach, 3u);
    // The bounce cells' passes, each built with only what it needs: the trace with the walk and
    // nothing else, the passes that find and keep cells without it.
    const auto bounce_pipeline = [&](string_view main_src, uint64_t tag, uint32_t cells) {
        return compose_deferred_compute_pipeline(ctx, main_src, tag, false, has_medium, render_view.bvh.ray_motion,
                                                 render_view.bvh.ray_motion_reach, cells);
    };
    auto bounce_trace_pipeline = bounce_pipeline(light_cells_bounce_trace_main_src, 0x4c43'4254'5243ULL, 3u);
    auto hits_pipeline = bounce_pipeline(light_cells_bounce_hits_main_src, 0x4c43'4248'4954ULL, 2u);
    auto gather_pipeline = bounce_pipeline(light_cells_bounce_gather_main_src, 0x4c43'4247'5448ULL, 4u);
    auto changes_pipeline = bounce_pipeline(light_cells_bounce_changes_main_src, 0x4c43'4243'4847ULL, 4u);
    auto arrive_pipeline = bounce_pipeline(light_cells_bounce_arrive_main_src, 0x4c43'4241'5256ULL, 2u);
    auto hops_pipeline = bounce_pipeline(light_cells_bounce_hops_main_src, 0x4c43'4248'4f50ULL, 4u);
    auto mark_pipeline = bounce_pipeline(light_cells_mark_main_src, 0x4c43'4d41'524bULL, 4u);
    // The pixels' reflection rays: the trace with the walk only, then the cells they hit.
    auto reflect_trace_pipeline = bounce_pipeline(light_cells_reflect_trace_main_src, 0x4c43'5246'5452ULL, 3u);
    auto reflect_hits_pipeline = bounce_pipeline(light_cells_reflect_hits_main_src, 0x4c43'5246'4849ULL, 2u);
    // The material where bounce and reflection rays hit, composed with the frame's materials
    // as the RT path's shading is.
    const auto material_pipeline = [&](string_view main_src, uint64_t tag) {
        IGpuPipeline::Ptr found;
        if (!ctx.snippets) return found;
        constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
        uint64_t key = 0xcbf29ce484222325ULL ^ tag;
        for (auto id : ctx.snippets->frame_materials()) key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
        for (auto id : ctx.snippets->frame_shadow_techs()) key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
        for (auto id : ctx.snippets->frame_intersects()) key = (key ^ static_cast<uint64_t>(id)) * kFnvPrime;
        key |= 0x2000000000000000ULL;
        found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
        if (!found) {
            found = ctx.render_ctx->pipelines().compile_compute(string_view(compose_rt_compute(*ctx.snippets, main_src)), key);
        }
        return found;
    };
    IGpuPipeline::Ptr shade_pipeline = material_pipeline(light_cells_bounce_shade_main_src, 0x4c43'4253'4844ULL);
    IGpuPipeline::Ptr reflect_shade_pipeline = material_pipeline(light_cells_reflect_shade_main_src, 0x4c43'5246'5348ULL);
    if (!pipeline || !trace_pipeline || !bounce_trace_pipeline || !hits_pipeline || !gather_pipeline || !changes_pipeline
        || !arrive_pipeline || !hops_pipeline || !mark_pipeline || !shade_pipeline || !reflect_trace_pipeline
        || !reflect_hits_pipeline || !reflect_shade_pipeline) {
        return;
    }

    DeferredComputePushC pc{};
    pc.globals_base = render_view.view_globals_base;
    pc.cam_pos[0] = render_view.cam_pos.x;
    pc.cam_pos[1] = render_view.cam_pos.y;
    pc.cam_pos[2] = render_view.cam_pos.z;
    pc.normal_tex_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
    pc.worldpos_tex_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
    pc.material_tex_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::MaterialParams));
    pc.width = vs.gbuffer_size.x;
    pc.height = vs.gbuffer_size.y;
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());
    pc.cells_header = static_cast<uint32_t>(vs.light_cell_header.offset() / sizeof(uint32_t));
    pc.lights_base = render_view.lights_base;
    pc.env_params[0] = render_view.env.intensity;
    pc.env_params[1] = render_view.env.rotation_rad;
    pc.env_texture_id = render_view.env.texture_id;

    bool dirty = true;
    emit_cached_view_pass(vs.light_cells_update_pass, dirty, "light_cells.update", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        // One pixel in each 2x2 block.
        dc.groups_x = ((vs.gbuffer_size.x + 1u) / 2u + 7u) / 8u;
        dc.groups_y = ((vs.gbuffer_size.y + 1u) / 2u + 7u) / 8u;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));
        // Three rounds: the large cells shown while smaller ones wait, then the other cells
        // not traced yet, then the rest with what is left of the list. See velk_cell_keep.
        DispatchCall gaps_dc = dc;
        DispatchCall rest_dc = dc;
        DeferredComputePushC round_pc = pc;
        round_pc.mode = 1;
        std::memcpy(gaps_dc.root_constants, &round_pc, sizeof(round_pc));
        round_pc.mode = 2;
        std::memcpy(rest_dc.root_constants, &round_pc, sizeof(round_pc));

        // What is listed, a group of threads an entry. Then the list is emptied.
        DeferredComputePushC trace_pc = pc;
        DispatchCall trace_dc{};
        trace_dc.pipeline = trace_pipeline.get();
        trace_dc.groups_x = light_cell_entries();
        trace_dc.groups_y = 1;
        trace_dc.groups_z = 1;
        trace_dc.root_constants_size = sizeof(trace_pc);
        std::memcpy(trace_dc.root_constants, &trace_pc, sizeof(trace_pc));
        DispatchCall empty_dc = trace_dc;
        empty_dc.groups_x = 1;
        trace_pc.mode = 2;
        std::memcpy(empty_dc.root_constants, &trace_pc, sizeof(trace_pc));
        // Before the rounds: last frame's gathered light into each cell's mean. A thread a slot.
        DispatchCall fold_dc = trace_dc;
        fold_dc.groups_x = kCellSlots / 64u;
        trace_pc.mode = 3;
        std::memcpy(fold_dc.root_constants, &trace_pc, sizeof(trace_pc));
        // First of all: what this frame's changes alter, marked on the cells. A thread a slot.
        DispatchCall mark_dc = fold_dc;
        mark_dc.pipeline = mark_pipeline.get();

        // The bounce cells over the hits of those pixels use are kept up and listed with the
        // room the pixels left. The bounce cells listed: their rays, a group a cell, then the
        // cells those hit, both before the direct light is traced so the cells found are traced
        // this frame too; then the material at the hits. After the list is emptied, every
        // bounce entry in use gathers, is checked against what changed, and brings arriving
        // light to its hits. See VELK_BOUNCE_GLSL.
        DispatchCall bounce_trace_dc = dc;
        bounce_trace_dc.pipeline = bounce_trace_pipeline.get();
        bounce_trace_dc.groups_x = kBounceListMax;
        bounce_trace_dc.groups_y = 1;
        DeferredComputePushC bounce_pc = pc;
        bounce_pc.mode = 4;
        bounce_pc.bounce_hops = light_cell_hops();
        std::memcpy(bounce_trace_dc.root_constants, &bounce_pc, sizeof(bounce_pc));
        DispatchCall hits_dc = bounce_trace_dc;
        hits_dc.pipeline = hits_pipeline.get();
        DispatchCall gather_dc = bounce_trace_dc;
        gather_dc.pipeline = gather_pipeline.get();
        gather_dc.groups_x = kBounceEntries;
        DispatchCall changes_dc = gather_dc;
        changes_dc.pipeline = changes_pipeline.get();
        DispatchCall arrive_dc = gather_dc;
        arrive_dc.pipeline = arrive_pipeline.get();
        arrive_dc.groups_x = kBounceEntries / 8u;
        // Two rounds, as the touch pass: new cells after the pixels' new, the rest after theirs.
        DispatchCall hops_new_dc = gather_dc;
        hops_new_dc.pipeline = hops_pipeline.get();
        DeferredComputePushC hops_pc = pc;
        hops_pc.mode = 1;
        std::memcpy(hops_new_dc.root_constants, &hops_pc, sizeof(hops_pc));
        DispatchCall hops_rest_dc = hops_new_dc;
        hops_pc.mode = 2;
        std::memcpy(hops_rest_dc.root_constants, &hops_pc, sizeof(hops_pc));
        BounceShadePushC shade_pc{};
        shade_pc.globals_base = render_view.view_globals_base;
        shade_pc.light_count = static_cast<uint32_t>(render_view.lights.size());
        shade_pc.lights_base = render_view.lights_base;
        shade_pc.env_params[0] = render_view.env.intensity;
        shade_pc.env_params[1] = render_view.env.rotation_rad;
        shade_pc.env[0] = render_view.env.material_id;
        shade_pc.env[1] = render_view.env.texture_id;
        shade_pc.extras[0] = static_cast<uint32_t>(vs.light_bounce_list.offset() / sizeof(uint32_t));
        shade_pc.extras[1] = kBounceListMax;
        DispatchCall shade_dc{};
        shade_dc.pipeline = shade_pipeline.get();
        shade_dc.groups_x = kBounceListMax;
        shade_dc.groups_y = 1;
        shade_dc.groups_z = 1;
        shade_dc.root_constants_size = sizeof(shade_pc);
        std::memcpy(shade_dc.root_constants, &shade_pc, sizeof(shade_pc));

        // The pixels' reflection rays: traced, a thread a pixel, then the cells they hit, both
        // before the direct light is traced; then the material at the hits.
        const bool reflect = vs.light_reflect.valid();
        DispatchCall reflect_trace_dc = dc;
        reflect_trace_dc.pipeline = reflect_trace_pipeline.get();
        reflect_trace_dc.groups_x = (vs.gbuffer_size.x + 7u) / 8u;
        reflect_trace_dc.groups_y = (vs.gbuffer_size.y + 7u) / 8u;
        std::memcpy(reflect_trace_dc.root_constants, &bounce_pc, sizeof(bounce_pc));
        DispatchCall reflect_hits_dc = reflect_trace_dc;
        reflect_hits_dc.pipeline = reflect_hits_pipeline.get();
        BounceShadePushC reflect_pc = shade_pc;
        reflect_pc.extras[0] = static_cast<uint32_t>(vs.light_reflect.offset() / sizeof(uint32_t));
        reflect_pc.extras[1] = vs.gbuffer_size.x | (vs.gbuffer_size.y << 16u);
        reflect_pc.extras[2] = pc.worldpos_tex_id;
        reflect_pc.extras[3] = pc.normal_tex_id;
        DispatchCall reflect_shade_dc = shade_dc;
        reflect_shade_dc.pipeline = reflect_shade_pipeline.get();
        reflect_shade_dc.groups_x = (vs.gbuffer_size.x * vs.gbuffer_size.y + 63u) / 64u;
        std::memcpy(reflect_shade_dc.root_constants, &reflect_pc, sizeof(reflect_pc));

        cmd->begin_recording();
        cmd->push_label("DeferredPath: light cells update");
        cmd->record_global_barrier();
        cmd->record_dispatch(mark_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(fold_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(gaps_dc);
        cmd->record_global_barrier();
        const bool hops = bounce_pc.bounce_hops >= 2u;
        if (hops) cmd->record_dispatch(hops_new_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(rest_dc);
        cmd->record_global_barrier();
        // Before the bounce trace, so the bounce entries reflections list are traced this
        // frame too.
        if (reflect) {
            cmd->record_dispatch(reflect_trace_dc);
            cmd->record_global_barrier();
            cmd->record_dispatch(reflect_hits_dc);
            cmd->record_global_barrier();
        }
        if (hops) cmd->record_dispatch(hops_rest_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(bounce_trace_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(hits_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(trace_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(shade_dc);
        cmd->record_global_barrier();
        if (reflect) {
            cmd->record_dispatch(reflect_shade_dc);
            cmd->record_global_barrier();
        }
        cmd->record_dispatch(empty_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(gather_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(changes_dc);
        cmd->record_global_barrier();
        cmd->record_dispatch(arrive_dc);
        // What reads the cells comes after.
        cmd->record_global_barrier();
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        rec.held.push_back(pipeline);
        rec.held.push_back(trace_pipeline);
        rec.held.push_back(bounce_trace_pipeline);
        rec.held.push_back(hits_pipeline);
        rec.held.push_back(gather_pipeline);
        rec.held.push_back(changes_pipeline);
        rec.held.push_back(arrive_pipeline);
        rec.held.push_back(hops_pipeline);
        rec.held.push_back(mark_pipeline);
        rec.held.push_back(reflect_trace_pipeline);
        rec.held.push_back(reflect_hits_pipeline);
        rec.held.push_back(reflect_shade_pipeline);
        rec.held.push_back(shade_pipeline);
    });
}

void DeferredPath::emit_light_cells_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                         FrameContext& ctx, uint32_t mode, IRenderGraph& graph)
{
    if (!vs.deferred_output || !vs.light_cell_header.valid()) return;

    const bool has_medium = render_view.bvh.has_medium != 0u;
    auto pipeline = compose_deferred_compute_pipeline(ctx, light_cells_main_src, 0x4c43'454c'4c53ULL, false, has_medium,
                                                      render_view.bvh.ray_motion, render_view.bvh.ray_motion_reach, 1u);
    if (!pipeline) return;

    IGpuTexture* src_tex = graph.resources().find_texture(vs.deferred_output.get());
    if (!src_tex) src_tex = vs.deferred_output_tex;
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) dst_tex = graph.resources().find_texture(color_target.get());
        if (!dst_tex) dst_tex = ctx.resources->find_texture(color_target.get());
    }

    // The deferred compute push constants. See CELLS_HEADER and CELLS_MODE.
    const auto attachment = [&](GBufferAttachment a) { return vs.gbuffer->attachment(static_cast<uint32_t>(a)); };
    DeferredComputePushC pc{};
    pc.globals_base = render_view.view_globals_base;
    pc.cam_pos[0] = render_view.cam_pos.x;
    pc.cam_pos[1] = render_view.cam_pos.y;
    pc.cam_pos[2] = render_view.cam_pos.z;
    pc.output_image_id = static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default));
    pc.albedo_tex_id = attachment(GBufferAttachment::Albedo);
    pc.normal_tex_id = attachment(GBufferAttachment::Normal);
    pc.worldpos_tex_id = attachment(GBufferAttachment::WorldPos);
    pc.material_tex_id = attachment(GBufferAttachment::MaterialParams);
    pc.width = vs.gbuffer_size.x;
    pc.height = vs.gbuffer_size.y;
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());
    pc.cells_header = static_cast<uint32_t>(vs.light_cell_header.offset() / sizeof(uint32_t));
    pc.lights_base = render_view.lights_base;
    pc.env_params[0] = render_view.env.intensity;
    pc.env_params[1] = render_view.env.rotation_rad;
    pc.mode = mode;

    bool dirty = true;
    emit_cached_view_pass(vs.light_cells_pass, dirty, "light_cells", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        DispatchCall dc{};
        dc.pipeline = pipeline.get();
        dc.groups_x = (vs.gbuffer_size.x + 7u) / 8u;
        dc.groups_y = (vs.gbuffer_size.y + 7u) / 8u;
        dc.groups_z = 1;
        dc.root_constants_size = sizeof(pc);
        std::memcpy(dc.root_constants, &pc, sizeof(pc));

        cmd->begin_recording();
        cmd->push_label("DeferredPath: light cells");
        cmd->record_global_barrier();
        cmd->record_dispatch(dc);
        cmd->record_global_barrier();
        if (src_tex && dst_tex) {
            cmd->record_blit_to_texture(*src_tex, *dst_tex, render_view.viewport);
        }
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.visibility));
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
        if (color_target) {
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        }
        rec.held.push_back(pipeline);
    });
}

void DeferredPath::emit_lighting_pass(IViewEntry& /*entry*/, ViewState& vs,
                                      const RenderView& render_view,
                                      IRenderTarget::Ptr color_target,
                                      FrameContext& ctx,
                                      int w, int h,
                                      IRenderGraph& graph)
{
    // Persistent allocation: keep the same Ptrs across frames so
    // downstream PushC bindless ids and `add_write` resource refs stay
    // stable. Recreate only on size change (or first-time). Recreation
    // invalidates the cached lighting pass (PushC ids change).
    uvec2 want{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    if (!vs.deferred_output || vs.output_size != want) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        // Lighting compute shader writes raw HDR linear radiance via
        // gStorageImagesF16 (binding 3). Tonemap, if requested, is a
        // separate post-process effect attached to the camera pipeline.
        td.format = PixelFormat::RGBA16F;
        td.usage = TextureUsage::Storage;
        vs.deferred_output = graph.resources().create_render_texture(td);
        vs.deferred_output_tex = graph.resources().find_texture(vs.deferred_output.get());
        vs.lighting_dirty = true;
    }
    if (!vs.deferred_output || !vs.light_cell_header.valid()) return;

    if (!vs.surface_light || vs.output_size != want) {
        TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = PixelFormat::RGBA16F;
        td.usage = TextureUsage::Storage;
        vs.surface_light = graph.resources().create_render_texture(td);
        vs.lighting_dirty = true;
    }
    if (!vs.surface_light) return;
    vs.output_size = want;

    IGpuTexture* src_tex = graph.resources().find_texture(vs.deferred_output.get());
    if (!src_tex) src_tex = vs.deferred_output_tex;
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) dst_tex = graph.resources().find_texture(color_target.get());
        if (!dst_tex && ctx.resources) dst_tex = ctx.resources->find_texture(color_target.get());
    }
    if (dst_tex != vs.last_output_dst) {
        vs.last_output_dst = dst_tex;
        vs.lighting_dirty = true;
    }

    // Which shader variant this view needs. Changing it changes the pipeline,
    // so the cached pass has to be re-recorded.
    bool has_area_light = false;
    for (const auto& l : render_view.lights) {
        if (l.flags[0] == static_cast<uint32_t>(LightType::Area)) {
            has_area_light = true;
            break;
        }
    }
    if (has_area_light != vs.had_area_light) {
        vs.had_area_light = has_area_light;
        vs.lighting_dirty = true;
    }

    const bool has_medium = render_view.bvh.has_medium != 0u;
    if (has_medium != vs.had_medium) {
        vs.had_medium = has_medium;
        vs.lighting_dirty = true;
    }

    auto lighting_pipeline = ensure_pipeline(ctx, has_area_light, has_medium, render_view.bvh.ray_motion,
                                             render_view.bvh.ray_motion_reach);
    if (!lighting_pipeline) return;

    auto albedo_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Albedo));
    auto normal_id   = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal));
    auto worldpos_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos));
    auto material_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::MaterialParams));
    auto emissive_id = vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Emissive));

    // Lights and env come pre-resolved from RenderView; shadows come from the light cells.
    DeferredComputePushC pc{};
    pc.globals_base = render_view.view_globals_base;
    pc.cam_pos[0] = render_view.cam_pos.x;
    pc.cam_pos[1] = render_view.cam_pos.y;
    pc.cam_pos[2] = render_view.cam_pos.z;
    pc.cam_pos[3] = 0.f;
    pc.output_image_id = static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default));
    pc.albedo_tex_id   = albedo_id;
    pc.normal_tex_id   = normal_id;
    pc.worldpos_tex_id = worldpos_id;
    pc.material_tex_id = material_id;
    pc.emissive_tex_id = emissive_id;
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    pc.light_count = static_cast<uint32_t>(render_view.lights.size());
    pc.env_texture_id = render_view.env.texture_id;
    pc.cells_header = static_cast<uint32_t>(vs.light_cell_header.offset() / sizeof(uint32_t));
    pc.surface_light_id = static_cast<uint32_t>(vs.surface_light->get_gpu_handle(GpuResourceKey::Default));
    // Zero means "no table", which the shader reads as diffuse-only area
    // lights rather than as a black highlight.
    const bool ltc_ready = ensure_ltc_tables(ctx, graph);
    pc.ltc_matrix_id = ltc_ready ? get_texture_id(ltc_matrix_) : 0u;
    pc.ltc_magnitude_id = ltc_ready ? get_texture_id(ltc_magnitude_) : 0u;
    pc.lights_base = render_view.lights_base;
    pc.env_params[0] = render_view.env.intensity;
    pc.env_params[1] = render_view.env.rotation_rad;

    // Writes the finished image and blits it to the target.
    emit_cached_view_pass(
        vs.cached_lighting_pass, vs.lighting_dirty, "deferred.lighting",
        graph,
        [&](CachedPassRecording& rec) {
            DispatchCall dc{};
            dc.pipeline = lighting_pipeline.get();
            dc.groups_x = (w + 7) / 8;
            dc.groups_y = (h + 7) / 8;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));

            if (auto cmd = ctx.backend->create_command_buffer()) {
                cmd->begin_recording();
                cmd->push_label("DeferredPath: lighting");
                cmd->record_dispatch(dc);
                if (src_tex && dst_tex) {
                    cmd->record_blit_to_texture(*src_tex, *dst_tex, render_view.viewport);
                }
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.surface_light));
            if (color_target) {
                rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
            }
            // Hold the lighting compute pipeline strong (cache is weak).
            rec.held.push_back(std::move(lighting_pipeline));
        });
}

void DeferredPath::emit_far_instance_pass(ViewState& vs, const RenderView& render_view, IRenderTarget::Ptr color_target,
                                          FrameContext& ctx, int w, int h, IRenderGraph& graph)
{
    if (!vs.deferred_output || !vs.gbuffer || !vs.surface_light || !render_view.batches) return;
    vector<const void*> done;
    vector<InstanceCullInfo> visuals;
    for (const auto& batch : *render_view.batches) {
        InstanceCullInfo info{};
        if (batch && batch->get_instance_cull(info) && info.far_form
            && std::find(done.begin(), done.end(), info.key) == done.end()) {
            done.push_back(info.key);
            visuals.push_back(info);
        }
    }
    if (visuals.empty()) return;

    auto arena = ctx.resources->shared_arena(IRenderBackend::kGlobalCulling, sizeof(uint32_t));
    if (!arena) return;

    // Per visual, frame-sized planes of coverage, then of coverage times each colour channel.
    const uint64_t bytes = uint64_t(w) * uint64_t(h) * sizeof(uint32_t) * 4u;
    vector<ViewState::FarInstanceWords> kept;
    for (const auto& info : visuals) {
        ViewState::FarInstanceWords entry;
        entry.key = info.key;
        for (auto& existing : vs.far_instance_words) {
            if (existing.key == info.key && existing.words.size() == bytes) {
                entry.words = std::move(existing.words);
            }
        }
        if (!entry.words.valid()) {
            entry.words = arena->alloc(bytes);
        }
        if (!entry.words.valid()) return;
        kept.push_back(std::move(entry));
    }
    vs.far_instance_words = std::move(kept);

    // The arena's buffer can only have changed above, and is named below.
    IGpuBuffer* arena_buffer = arena->buffer();
    if (!arena_buffer) return;

    const auto pipeline_for = [&](string_view src, uint64_t key) {
        auto found = ctx.render_ctx->pipelines().find(PipelineCacheKey{key, PixelFormat::RGBA8, DepthFormat::None, 0});
        return found ? found : ctx.render_ctx->pipelines().compile_compute(src, key);
    };
    auto splat = pipeline_for(instance_splat_src, 0x5350'4c41'5400'0001ULL);
    auto composite = pipeline_for(instance_splat_composite_src, 0x5350'4c41'5400'0002ULL);
    auto box_splat = pipeline_for(instance_box_splat_src, 0x5350'4c41'5400'0003ULL);
    if (!splat || !composite || !box_splat) return;

    IGpuTexture* src_tex = graph.resources().find_texture(vs.deferred_output.get());
    if (!src_tex) src_tex = vs.deferred_output_tex;
    IGpuTexture* dst_tex = nullptr;
    if (color_target) {
        dst_tex = interface_cast<IGpuTexture>(color_target.get());
        if (!dst_tex) dst_tex = graph.resources().find_texture(color_target.get());
        if (!dst_tex) dst_tex = ctx.resources->find_texture(color_target.get());
    }

    const auto words_word = [&](size_t i) {
        return static_cast<uint32_t>(vs.far_instance_words[i].words.offset() / sizeof(uint32_t));
    };
    // Re-recorded each frame: one dispatch per visual and a blit.
    bool dirty = true;
    emit_cached_view_pass(vs.far_instance_pass, dirty, "instance.splat", graph, [&](CachedPassRecording& rec) {
        auto cmd = ctx.backend->create_command_buffer();
        if (!cmd) return;
        cmd->begin_recording();
        cmd->push_label("Far instances");
        cmd->record_global_barrier();
        for (const auto& region : vs.far_instance_words) {
            cmd->record_clear_buffer(arena_buffer, region.words.offset(), bytes);
        }
        cmd->record_global_barrier();
        struct SplatPush
        {
            uint32_t instances_base, slot_count, table_word, globals_base, chunks_word, words_word;
            uint32_t width, height, worldpos_id, normal_id;
        };
        for (size_t i = 0; i < visuals.size(); ++i) {
            const auto& info = visuals[i];
            const bool partitioned = info.partition_count != 0;
            const SplatPush pc{info.instances_base, info.slot_count, info.table_word, render_view.view_globals_base,
                               partitioned ? info.chunk_list_word + kCullChunkListHeaderWords : kCullNoSlot,
                               words_word(i), static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                               vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos)),
                               vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal))};
            DispatchCall dc{};
            dc.pipeline = splat.get();
            dc.groups_x = (info.slot_count + 63u) / 64u;
            dc.groups_y = 1;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            // The cull pass left the count of partitions' chunks in view.
            if (partitioned) {
                cmd->record_dispatch_indirect(dc, arena_buffer, uint64_t(info.chunk_list_word) * sizeof(uint32_t));
            } else {
                cmd->record_dispatch(dc);
            }

            if (info.has_unloaded) {
                struct BoxPush
                {
                    uint32_t globals_base, words_word, width, height, worldpos_id, normal_id, medium_id;
                };
                const BoxPush box{render_view.view_globals_base, words_word(i), static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                  vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos)),
                                  vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal)), info.medium_id};
                DispatchCall box_dc{};
                box_dc.pipeline = box_splat.get();
                box_dc.groups_x = (static_cast<uint32_t>(w) + 7u) / 8u;
                box_dc.groups_y = (static_cast<uint32_t>(h) + 7u) / 8u;
                box_dc.groups_z = 1;
                box_dc.root_constants_size = sizeof(box);
                std::memcpy(box_dc.root_constants, &box, sizeof(box));
                cmd->record_dispatch(box_dc);
            }
        }
        cmd->record_global_barrier();
        struct CompositePush
        {
            uint32_t words_word, width, height, output_id, surface_light_id, normal_id, worldpos_id;
            uint32_t globals_base, lights_base, light_count;
            float facing[kSurfaceDirectionCount];
        };
        static_assert(sizeof(CompositePush) <= kMaxRootConstantsSize);
        for (size_t i = 0; i < visuals.size(); ++i) {
            const auto& info = visuals[i];
            CompositePush pc{words_word(i), static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                             static_cast<uint32_t>(vs.deferred_output->get_gpu_handle(GpuResourceKey::Default)),
                             static_cast<uint32_t>(vs.surface_light->get_gpu_handle(GpuResourceKey::Default)),
                             vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::Normal)),
                             vs.gbuffer->attachment(static_cast<uint32_t>(GBufferAttachment::WorldPos)),
                             render_view.view_globals_base, render_view.lights_base,
                             static_cast<uint32_t>(render_view.lights.size()),
                             {}};
            std::memcpy(pc.facing, info.far_facing, sizeof(pc.facing));
            DispatchCall dc{};
            dc.pipeline = composite.get();
            dc.groups_x = (static_cast<uint32_t>(w) + 7u) / 8u;
            dc.groups_y = (static_cast<uint32_t>(h) + 7u) / 8u;
            dc.groups_z = 1;
            dc.root_constants_size = sizeof(pc);
            std::memcpy(dc.root_constants, &pc, sizeof(pc));
            cmd->record_dispatch(dc);
            cmd->record_global_barrier();
        }
        if (src_tex && dst_tex) {
            cmd->record_blit_to_texture(*src_tex, *dst_tex, render_view.viewport);
        }
        cmd->pop_label();
        cmd->end_recording();
        rec.cmd = std::move(cmd);
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
        rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.surface_light));
        rec.writes.push_back(interface_pointer_cast<IGpuResource>(vs.deferred_output));
        if (color_target) {
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        }
        rec.held.push_back(std::move(splat));
        rec.held.push_back(std::move(composite));
        rec.held.push_back(std::move(box_splat));
    });
}

void DeferredPath::emit_transparent_pass(IViewEntry& /*entry*/, ViewState& vs,
                                         const RenderView& render_view,
                                         IRenderTarget::Ptr color_target,
                                         FrameContext& ctx,
                                         IRenderGraph& graph)
{
    if (!render_view.batches || render_view.batches->empty()) return;
    if (!color_target || !vs.visibility || !ctx.backend || !ctx.frame_buffer) return;

    // Resolve the composite target as an IGpuTexture (surface composite casts
    // directly; a RenderTexture proxy needs find_texture).
    IGpuTexture* target_texture = interface_cast<IGpuTexture>(color_target.get());
    if (!target_texture) {
        target_texture = graph.resources().find_texture(color_target.get());
        if (!target_texture && ctx.resources) {
            target_texture = ctx.resources->find_texture(color_target.get());
        }
    }
    if (!target_texture) return;

    IGpuTexture* depth_texture = vs.visibility->depth_attachment();

    // Retarget / resize detection: the cached cmd buffer bakes the composite
    // VkImage handle.
    if (target_texture != vs.last_transparent_target) {
        vs.transparent_dirty = true;
        vs.last_transparent_target = target_texture;
    }

    emit_cached_view_pass(
        vs.cached_transparent_pass, vs.transparent_dirty, "deferred.transparent",
        graph,
        [&](CachedPassRecording& rec) {
            auto* default_uv1 =
                ctx.render_ctx->get_default_buffer(DefaultBufferType::Uv1).get();
            const PixelFormat target_format = ctx.target_format;
            const DepthFormat depth_format = color_target->get_depth_format();

            // Only BLEND batches; everything else drew opaque in the gbuffer.
            // resolve returns null to skip non-blended batches, and holds the
            // pipeline strong (the cache is weak).
            auto resolve = [&](const IBatch& b) -> IGpuPipeline* {
                if (b.pipeline_options().blend_mode != BlendMode::Alpha) {
                    return nullptr;
                }
                // Glass driver: Fresnel-boosted opacity at grazing angles.
                auto p = resolve_or_compile_forward(
                    *ctx.render_ctx, b, target_format, depth_format,
                    transparent_fragment_driver_template, kTransparentEvalContentTag);
                if (!p) return nullptr;
                IGpuPipeline* raw = p.get();
                rec.held.push_back(std::move(p));
                return raw;
            };

            vector<DrawCall> draw_calls;
            emit_draw_calls(
                draw_calls, *render_view.batches, *ctx.frame_buffer, *ctx.resources,
                default_uv1, render_view.view_globals_base, resolve,
                render_view.has_frustum ? &render_view.frustum : nullptr);

            // No transparent geometry this view: leave an empty (no-op) pass.
            if (draw_calls.empty()) return;

            if (auto cmd = ctx.backend->create_command_buffer()) {
                // Load the lit composite + the opaque gbuffer depth; blend the
                // transparent draws over them (pipelines carry BlendMode::Alpha
                // and depth_write=false from the materials' options).
                ColorAttachment color{};
                color.texture = target_texture;
                color.clear = false;

                DepthAttachment depth_att{};
                depth_att.texture = depth_texture;
                depth_att.clear = false;

                cmd->begin_recording();
                cmd->push_label("DeferredPath: transparent");
                cmd->record_begin_rendering(
                    array_view<const ColorAttachment>(&color, 1),
                    depth_texture ? &depth_att : nullptr);
                cmd->set_viewport(render_view.viewport);
                cmd->record_draws({draw_calls.data(), draw_calls.size()});
                cmd->record_end_rendering();
                cmd->pop_label();
                cmd->end_recording();
                rec.cmd = std::move(cmd);
            }

            // Reads the gbuffer (its depth); reads + writes the composite.
            rec.reads.push_back(interface_pointer_cast<IGpuResource>(vs.gbuffer));
            rec.writes.push_back(interface_pointer_cast<IGpuResource>(color_target));
        });
}

DeferredPath::~DeferredPath()
{
    // Detach from every view we observed. The renderer's destruction
    // order keeps IViewEntry::Ptrs alive past path destruction, so
    // these calls are safe.
    for (auto& [view, _] : view_states_) {
        view->remove_render_state_observer(this);
    }
}

void DeferredPath::on_render_state_changed(IRenderState* source,
                                           RenderStateChange /*flags*/)
{
    auto* view = interface_cast<IViewEntry>(source);
    if (!view) return;
    auto it = view_states_.find(view);
    if (it != view_states_.end()) {
        it->second.visibility_dirty = true;
        it->second.lighting_dirty = true;
        it->second.transparent_dirty = true;
        it->second.instance_cull.dirty = true;
    }
}

void DeferredPath::on_view_removed(IViewEntry& entry, FrameContext& ctx)
{
    if (forward_fallback_) {
        forward_fallback_->on_view_removed(entry, ctx);
    }
    auto it = view_states_.find(&entry);
    if (it == view_states_.end()) return;
    entry.remove_render_state_observer(this);
    // Erase the view state; gbuffer / deferred_output Ptrs drop, resource manager
    // auto-defers the backend handles.
    view_states_.erase(it);
}

void DeferredPath::shutdown(FrameContext& ctx)
{
    if (forward_fallback_) {
        forward_fallback_->shutdown(ctx);
        forward_fallback_ = nullptr;
    }
    for (auto& [view, _] : view_states_) {
        view->remove_render_state_observer(this);
    }
    view_states_.clear();
}

IGpuResource::Ptr DeferredPath::find_named_output(string_view name, IViewEntry* view) const
{
    auto it = view_states_.find(view);
    if (it == view_states_.end()) return {};
    auto& vs = it->second;
    if (name == "gbuffer") {
        return interface_pointer_cast<IGpuResource>(vs.gbuffer);
    }
    auto wrap = [](IGpuTexture* tex) -> IGpuResource::Ptr {
        return tex ? IGpuResource::Ptr(static_cast<IGpuResource*>(tex))
                   : IGpuResource::Ptr{};
    };
    if (name == "gbuffer.worldpos") {
        // Return the underlying IGpuTexture (cast as IGpuResource) so
        // debug code can `interface_cast<IGpuTexture>` and dump it.
        return wrap(vs.gbuffer
                ? vs.gbuffer->attachment_texture(
                      static_cast<uint32_t>(GBufferAttachment::WorldPos))
                : nullptr);
    }
    if (name == "output") {
        return wrap(vs.deferred_output_tex);
    }
    return {};
}

} // namespace velk
