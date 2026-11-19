// The verification app: a set of scenes, from one box under a sun to large instanced fields,
// each with a few fixed views, for looking at the renderer's structures and for checking its
// images against references.
//
// velk_ui_inspect [scene=NAME] [view=NAME] [level=N] [at=X,Y,Z,YAW,PITCH] [path[=FRAME]]
// velk_ui_inspect verify [scene=NAMES] [skip=NAMES] [settle=N]
// velk_ui_inspect reference [scene=NAMES] [skip=NAMES] [frames=N]
// velk_ui_inspect trend [scene=NAMES]
//
// NAMES is a comma separated list of scenes: scene= runs only those, skip= leaves those out.
//
// Interactive (the default): one scene (scene=, the field by default) at its first view under
// free control, with an overview from above drawing what the main camera draws. Right drag
// turns, W, A, S and D move, the wheel moves along the view. Keys:
//   V  next debug view (lit, cluster, level, cluster size, instance, triangle size, partition,
//      ray tree, ray top tree, quads, record, light)
//   K, L  the ray tree views' level down and up
//   P  scripted path on or off, the same motion every time
//   C  logs the camera's place, as at= takes it
//
// verify: every scene at each of its views: loads for 10 frames, and what it moves moves; then
// `settle` frames (10), measuring how much changes from frame to frame from the load on, or
// from the movement's end, and by which frame it stops, the first of those frames judged
// against the reference too; then over 8 more
// frames how far the image is from the view's reference, how much it still changes, the GPU
// time and memory. In a scene that moves things, five frames of the movement are each judged
// against their own reference. A view with a motion then moves, a step a frame, each step
// against its own reference, and comes back to be compared with how it looked before. Writes, overwriting the
// last run, the images, their difference maps and a report to build/verify; then exits. GPU
// times are checked against the median of the view's last runs on committed code in the
// history (reference_images/verify/timings.tsv), and a run of every scene adds its times to
// it, still and moving, each pass's too, with the commits of velk-ui and velk.
//
// trend: each view's times in the history, oldest first, marking where they stepped.
//
// reference: every scene at each of its views, each step of a view's motion, and what a scene
// moves held at each judged frame of its movement, with the reference path, `frames` frames (128) accumulated each, saved in reference_images/verify;
// then exits. Make them again only when a scene changes.
//
// Up is -y, as in the other samples.

#include <velk/api/velk.h>

#include "scenes.h"
#include "timings.h"
#include "verify.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <utility>
#include <velk-render/api/render_context.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/post_process.h>
#include <velk-scene/api/render_path.h>
#include <velk-scene/api/scene.h>
#include <velk-scene/api/trait/orbit.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/plugins/image/plugin.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/api/text_visual.h>

#ifndef INSPECT_REFERENCE_DIR
#define INSPECT_REFERENCE_DIR "reference_images/verify"
#endif
#ifndef INSPECT_RUN_DIR
#define INSPECT_RUN_DIR "verify"
#endif
#ifndef INSPECT_SOURCE_DIR
#define INSPECT_SOURCE_DIR "."
#endif
#ifndef INSPECT_VELK_DIR
#define INSPECT_VELK_DIR "../velk"
#endif
/// The parts of each repository whose changes count as code verify measures: a run with
/// changes elsewhere still counts as on committed code.
#define INSPECT_MEASURED_PATHS "velk-render velk-runtime velk-scene velk-ui plugins samples/inspect"
#define INSPECT_VELK_MEASURED_PATHS "velk"

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// A camera that flies: an orbit at distance 1 around a focus that moves.
struct FlyCamera
{
    velk::Element element;
    velk::Camera camera;
    velk::OrbitTrait orbit;
    velk::Trs focus;
};

FlyCamera add_camera(velk::Scene& scene, velk::Element& root, bool reference)
{
    FlyCamera fly;
    fly.element = velk::create_element();
    auto focus = velk::create_element();
    fly.focus = velk::trait::transform::create_trs();
    focus.add_trait(fly.focus);
    scene.add(root, focus);
    scene.add(root, fly.element);

    auto cam_trait = velk::trait::render::create_camera();
    fly.camera = velk::Camera(cam_trait);
    fly.camera.set_projection(velk::Projection::Perspective);
    fly.camera.set_fov(50.f);
    fly.camera.add_render_path(reference ? velk::path::create_reference() : velk::path::create_deferred());
    auto post = velk::pp::create_post_process();
    post.add(velk::pp::create_tonemap());
    fly.camera.add_post_process(post);
    fly.element.add_trait(cam_trait);

    fly.orbit = velk::trait::transform::create_orbit();
    fly.orbit.set_target(focus);
    fly.orbit.set_distance(1.f);
    fly.element.add_trait(fly.orbit);
    fly.element.find_trait<velk::ICamera>()->far_clip().set_value(40000.f);
    return fly;
}

void place_camera(FlyCamera& fly, const inspect::Place& place)
{
    fly.focus.set_translate(place.at);
    fly.orbit.set_yaw(place.yaw);
    fly.orbit.set_pitch(place.pitch);
}

struct NamedView
{
    velk::DebugView view;
    const char* key;     ///< As view= takes it.
    const char* name;
    const char* legend;  ///< What the colours mean.
};

constexpr NamedView kViews[] = {
    {velk::DebugView::None, "lit", "lit", "the lit image"},
    {velk::DebugView::Cluster, "cluster", "cluster",
     "a colour per level of detail cluster of each instance; grey: not drawn by cluster"},
    {velk::DebugView::Level, "level", "level",
     "level of detail: red full detail, then orange, yellow, green, cyan, blue, purple, pink"},
    {velk::DebugView::ClusterSize, "size", "cluster size",
     "cluster width on the main camera's screen: blue <8 px, cyan <16, green <32, yellow <64, orange <128, red more"},
    {velk::DebugView::Instance, "instance", "instance", "a colour per instance"},
    {velk::DebugView::TriangleSize, "triangles", "triangle size",
     "triangle size on screen: magenta drawn in compute, red <0.5 px, yellow <1, green <2, cyan <8, blue more"},
    {velk::DebugView::Partition, "partition", "partition",
     "partition bounds: red while a partition chooses its levels of detail again, cyan while it keeps them"},
    {velk::DebugView::RayTree, "tree", "ray tree",
     "boxes of one level of the trees rays walk over each partition (K and L for the level): 0 four clusters each"},
    {velk::DebugView::RayTop, "top", "ray top tree",
     "boxes of one level of the tree rays walk over the partitions (K and L for the level): 0 one partition each"},
    {velk::DebugView::Quads, "quads", "quads",
     "the quads the cut chose in their colour, everything else grey"},
    {velk::DebugView::Record, "record", "record",
     "a colour per kept cut record what is drawn is from, white for none"},
    {velk::DebugView::Light, "light", "light points",
     "a colour per point of light, blended as pixels read them, darker for a record's own points; flat: one value; quads by light node; magenta not traced yet; cyan stale since a change; green an exact coarse cell, finer points not traced; yellow a cell whose corners are not known yet; grey none"},
};
constexpr size_t kViewCount = sizeof(kViews) / sizeof(kViews[0]);

// What a verification run accepts, against the reference and frame to frame. Starting values,
// to be tightened as the scenes pass.
constexpr uint32_t kCompareScale = 4;        ///< Images are compared this many pixels to one.
constexpr float kMaxMean = 0.02f;            ///< Mean luminance difference.
constexpr float kMaxP95 = 0.08f;             ///< 95th percentile of it.
constexpr uint32_t kTilePixels = 32;         ///< The squares judged on their own, in the window's pixels.
constexpr float kMaxTile = 0.04f;            ///< Mean luminance difference over the worst square.
constexpr float kChangeThreshold = 0.1f;     ///< A pixel this much brighter or darker in a frame has changed.
constexpr float kMaxChangeShare = 0.0005f;   ///< Share of pixels changing in a frame of a still view.
constexpr long kMeasureFrames = 8;
constexpr long kSettleFrom = 10;             ///< Frame changes count from here while a view settles.
constexpr long kBackFrames = 60;             ///< Frames back at a view after its motion, before it is compared.
constexpr size_t kBaselineRuns = 5;          ///< Runs on committed code a view's times are measured against.
constexpr int kDuringSamples = 5;            ///< Frames of a scene's movement judged against references.

/// The frame of a scene's movement where sample @p k (1 to kDuringSamples) is judged, evenly
/// spaced, neither its start nor its end.
long during_frame(const inspect::BuiltScene& built, int k)
{
    return built.frames * k / (kDuringSamples + 1);
}

/// One scene at one view, as a verification or reference run goes through them.
struct Job
{
    size_t scene;
    size_t view;
};

velk::string reference_path(const inspect::SceneSpec& spec, const inspect::ViewSpec& view, const char* ext)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s/%s_%s%s", INSPECT_REFERENCE_DIR, spec.name, view.name, ext);
    return velk::string(buf);
}

velk::string run_path(const inspect::SceneSpec& spec, const inspect::ViewSpec& view, const char* ext)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s/%s_%s%s", INSPECT_RUN_DIR, spec.name, view.name, ext);
    return velk::string(buf);
}

/// `s` as a view of its characters, without the terminating zero.
velk::string_view view_of(const char* s)
{
    return velk::string_view(s, std::strlen(s));
}

/// The history of verification runs' GPU times.
constexpr const char* kTimingsPath = INSPECT_REFERENCE_DIR "/timings.tsv";

/// A frame's GPU time, all passes.
double frame_ms(velk::IRenderBackend& backend)
{
    double ms = 0.0;
    for (const auto& t : backend.last_gpu_timings()) ms += t.ms;
    return ms;
}

} // namespace

int main(int argc, char* argv[])
{
    enum class Mode { Interactive, Verify, Reference, Trend };
    Mode mode = Mode::Interactive;
    const char* only_scene = nullptr;
    const char* skip_scenes = nullptr;
    size_t start_view = 0;
    bool has_place = false;
    float place[5] = {};
    bool start_scripted = false;
    long start_frame = 0;
    uint32_t tree_level = 0;
    long settle_frames = 10;
    long reference_frames = 128;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "verify") == 0) {
            mode = Mode::Verify;
        } else if (std::strcmp(arg, "reference") == 0) {
            mode = Mode::Reference;
        } else if (std::strcmp(arg, "trend") == 0) {
            mode = Mode::Trend;
        } else if (std::strncmp(arg, "scene=", 6) == 0) {
            only_scene = arg + 6;
        } else if (std::strncmp(arg, "skip=", 5) == 0) {
            skip_scenes = arg + 5;
        } else if (std::strncmp(arg, "settle=", 7) == 0) {
            settle_frames = std::max(1l, std::atol(arg + 7));
        } else if (std::strncmp(arg, "frames=", 7) == 0) {
            reference_frames = std::max(1l, std::atol(arg + 7));
        } else if (std::strncmp(arg, "level=", 6) == 0) {
            tree_level = static_cast<uint32_t>(std::atol(arg + 6));
        } else if (std::strncmp(arg, "view=", 5) == 0) {
            bool known = false;
            for (size_t v = 0; v < kViewCount; ++v) {
                if (std::strcmp(arg + 5, kViews[v].key) == 0) {
                    start_view = v;
                    known = true;
                }
            }
            if (!known) {
                VELK_LOG(E, "inspect: unknown view '%s'", arg + 5);
            }
        } else if (std::strncmp(arg, "at=", 3) == 0) {
            has_place = std::sscanf(arg + 3, "%f,%f,%f,%f,%f", &place[0], &place[1], &place[2], &place[3], &place[4]) == 5;
            if (!has_place) {
                VELK_LOG(E, "inspect: at= takes X,Y,Z,YAW,PITCH");
            }
        } else if (std::strcmp(arg, "path") == 0) {
            start_scripted = true;
        } else if (std::strncmp(arg, "path=", 5) == 0) {
            start_scripted = true;
            start_frame = std::atol(arg + 5);
        } else {
            VELK_LOG(E, "inspect: unknown argument '%s'", arg);
        }
    }
    const bool interactive = mode == Mode::Interactive;

    // The trend needs no window: the history, printed.
    if (mode == Mode::Trend) {
        const auto rows = inspect::load_timings(view_of(kTimingsPath));
        if (rows.empty()) {
            std::printf("no history in %s\n", kTimingsPath);
            return 0;
        }
        inspect::print_trend(
            rows,
            [](const char* scene, void* user) {
                const char* list = static_cast<const char*>(user);
                if (!list) return true;
                const size_t length = std::strlen(scene);
                for (const char* at = list; at && *at;) {
                    const char* end = std::strchr(at, ',');
                    const size_t n = end ? static_cast<size_t>(end - at) : std::strlen(at);
                    if (n == length && std::strncmp(at, scene, n) == 0) return true;
                    at = end ? end + 1 : nullptr;
                }
                return false;
            },
            const_cast<char*>(only_scene));
        return 0;
    }

    velk::ApplicationConfig config;
    velk::WindowConfig wc;
    wc.width = 1280;
    wc.height = 720;
    wc.title = "velk-ui inspect";
    wc.depth = velk::DepthFormat::Default;
    wc.color_format = velk::SurfaceColorFormat::RGBA16F;

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "inspect: failed to create window");
        return 1;
    }
    auto ctx = app.render_context();
    auto render_context = static_cast<velk::IRenderContext::Ptr>(ctx);
    if (!render_context) {
        VELK_LOG(E, "inspect: no render context");
        return 1;
    }

    // The scene shown, its root and its camera. Each scene a run goes to is built into a scene
    // of its own, with a camera and view of its own, so that nothing one leaves carries over.
    velk::Scene scene;
    velk::Element root;
    FlyCamera main_camera;

    // The scenes and views a run goes through, in order: those scene= names (a comma separated
    // list, all by default), less those skip= names.
    const auto& specs = inspect::scenes();
    const auto listed = [](const char* list, const char* name) {
        const size_t length = std::strlen(name);
        for (const char* at = list; at && *at;) {
            const char* end = std::strchr(at, ',');
            const size_t n = end ? static_cast<size_t>(end - at) : std::strlen(at);
            if (n == length && std::strncmp(at, name, n) == 0) return true;
            at = end ? end + 1 : nullptr;
        }
        return false;
    };
    for (const char* list : {only_scene, skip_scenes}) {
        for (const char* at = list; at && *at;) {
            const char* end = std::strchr(at, ',');
            char name[64] = {};
            std::strncpy(name, at, std::min<size_t>(end ? static_cast<size_t>(end - at) : std::strlen(at), sizeof(name) - 1));
            if (!inspect::find_scene(name)) {
                VELK_LOG(E, "inspect: unknown scene '%s'", name);
                return 1;
            }
            at = end ? end + 1 : nullptr;
        }
    }
    velk::vector<Job> jobs;
    for (size_t s = 0; s < specs.size(); ++s) {
        if ((only_scene && !listed(only_scene, specs[s].name)) || listed(skip_scenes, specs[s].name)) continue;
        for (size_t v = 0; v < specs[s].views.size(); ++v) jobs.push_back({s, v});
    }
    const inspect::SceneSpec* interactive_spec = jobs.empty() ? inspect::find_scene("field") : &specs[jobs[0].scene];

    size_t built_index = static_cast<size_t>(-1);
    inspect::BuiltScene built;
    const auto show_scene = [&](size_t index) {
        if (built_index == index) return;
        auto next_scene = velk::create_scene();
        next_scene.set_geometry(velk::aabb::from_size({static_cast<float>(wc.width), static_cast<float>(wc.height)}));
        auto next_root = velk::create_element();
        next_scene.set_root(next_root);
        FlyCamera next_camera = add_camera(next_scene, next_root, mode == Mode::Reference);
        built = inspect::build_scene(specs[index], next_scene, next_root, ctx, next_camera.camera, mode == Mode::Reference);
        // A run without a window captures the view= asked for.
        if (!interactive) next_camera.camera.set_debug_view(kViews[start_view].view);
        // The new view before the old one goes, so that the window's surface stays.
        app.add_view(window, next_camera.element);
        if (main_camera.element) app.remove_view(window, main_camera.element);
        scene = next_scene;
        root = next_root;
        main_camera = next_camera;
        built_index = index;
    };

    if (!interactive) {
        return [&]() {
            auto backend = render_context->backend();
            auto writer = velk::instance().create<velk::IImageWriter>(velk::ui::ClassId::ImageEncoder);
            if (!backend || !writer) {
                VELK_LOG(E, "inspect: no backend or image writer");
                return 1;
            }
            std::error_code ec;
            std::filesystem::create_directories(INSPECT_REFERENCE_DIR, ec);
            if (mode == Mode::Verify) {
                // The last run's files go, so that what is there is this run's.
                std::filesystem::remove_all(INSPECT_RUN_DIR, ec);
                std::filesystem::create_directories(INSPECT_RUN_DIR, ec);
                backend->set_gpu_timing_enabled(true);
            }
            const uint64_t surface_id = velk::get_render_target_id(window.surface());

            velk::IGpuResourceManager* resources = render_context->resources();
            velk::string report;
            const auto line = [&](const char* text) {
                VELK_LOG(I, "verify: %s", text);
                report.append(text);
                report.append("\n");
            };
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%-24s %7s %7s %7s %8s %7s %8s %7s %7s %11s %7s %7s %7s  %s", "scene/view", "mean",
                          "p95", "tile", "burst", "settled", "changed", "gpu ms", "was", "moving ms", "gpu MB", "arenas", "culling",
                          "verdict");
            if (mode == Mode::Verify) line(buf);
            // The history the times are checked against, and this run's, which a run of every
            // scene adds to it.
            const velk::vector<inspect::TimingRow> history =
                mode == Mode::Verify ? inspect::load_timings(view_of(kTimingsPath)) : velk::vector<inspect::TimingRow>{};
            velk::vector<inspect::ViewTimes> run_times;
            // A time against its baseline: "-" without one.
            const auto was_text = [](double was, char* out, size_t size) {
                if (was > 0.0) std::snprintf(out, size, "%.2f", was);
                else std::snprintf(out, size, "-");
            };
            const auto moving_text = [](const velk::vector<double>& ms, double& median, double& worst, char* out, size_t size) {
                median = -1.0;
                worst = -1.0;
                if (ms.empty()) {
                    std::snprintf(out, size, "-");
                    return;
                }
                velk::vector<double> sorted = ms;
                std::sort(sorted.begin(), sorted.end());
                median = sorted[sorted.size() / 2];
                worst = sorted.back();
                std::snprintf(out, size, "%.2f/%.2f", median, worst);
            };
            // The last frame's passes added to `into`, and `into` as averages over `frames`.
            const auto add_passes = [&](velk::vector<inspect::PassTime>& into) {
                for (const auto& t : backend->last_gpu_timings()) {
                    auto found = std::find_if(into.begin(), into.end(), [&](const inspect::PassTime& p) { return p.name == t.label; });
                    if (found == into.end()) into.push_back({velk::string(t.label), double(t.ms)});
                    else found->ms += t.ms;
                }
            };
            const auto average_passes = [](velk::vector<inspect::PassTime>& passes, size_t frames) {
                for (auto& p : passes) p.ms /= static_cast<double>(frames);
            };
            // Where a slower view's time went: the passes that grew most since the baseline's run.
            const auto slower_passes = [&](const char* what, const velk::vector<inspect::PassTime>& now,
                                           const velk::vector<inspect::PassTime>& was, const velk::string& since) {
                struct Growth
                {
                    const char* name;
                    double was;
                    double now;
                };
                velk::vector<Growth> grown;
                for (const auto& p : now) {
                    double before = 0.0;
                    for (const auto& q : was) {
                        if (q.name == p.name) before = q.ms;
                    }
                    if (p.ms > before) grown.push_back({p.name.c_str(), before, p.ms});
                }
                std::sort(grown.begin(), grown.end(), [](const Growth& a, const Growth& b) { return a.now - a.was > b.now - b.was; });
                velk::string text("  ");
                text.append(view_of(what));
                text.append(" since ");
                text.append(since);
                text.append(":");
                for (size_t i = 0; i < grown.size() && i < 3u; ++i) {
                    char part[160];
                    std::snprintf(part, sizeof(part), "%s %s %.2f to %.2f ms", i == 0 ? "" : ",", grown[i].name, grown[i].was, grown[i].now);
                    text.append(part);
                }
                line(text.c_str());
            };
            size_t passed = 0;
            size_t failed = 0;
            double peak_mb = 0.0;
            double peak_arenas_mb = 0.0;
            double peak_cull_mb = 0.0;
            char peak_at[64] = "";

            // One frame of the run. False when the window closed.
            const auto frame = [&]() {
                if (!app.poll()) {
                    VELK_LOG(E, "inspect: window closed during the run");
                    return false;
                }
                app.update();
                app.present();
                return true;
            };
            const auto grab = [&](inspect::Image& out) {
                if (!inspect::grab_window(*backend, surface_id, out)) {
                    VELK_LOG(E, "inspect: could not read the window back");
                    return false;
                }
                return true;
            };
            // A row's verdict from a comparison, with what else failed.
            const auto verdict_of = [&](const inspect::Difference& d, const char* also, char* out, size_t size) {
                char where[48] = "";
                if (d.worst_tile > kMaxTile) {
                    std::snprintf(where, sizeof(where), " tile (at %u, %u px)", d.tile_x * kTilePixels, d.tile_y * kTilePixels);
                }
                if (d.mean > kMaxMean || d.p95 > kMaxP95 || d.worst_tile > kMaxTile || also[0] != '\0') {
                    std::snprintf(out, size, "FAIL:%s%s%s%s", d.mean > kMaxMean ? " mean" : "", d.p95 > kMaxP95 ? " p95" : "",
                                  where, also);
                } else {
                    std::snprintf(out, size, "ok");
                }
            };
            const auto count = [&](const char* verdict) { (std::strcmp(verdict, "ok") == 0 ? passed : failed) += 1; };
            const size_t tile = kTilePixels / kCompareScale;

            for (const Job& job : jobs) {
                const inspect::SceneSpec& spec = specs[job.scene];
                const inspect::ViewSpec& view = spec.views[job.view];
                show_scene(job.scene);
                place_camera(main_camera, view.place);
                char name[64];
                std::snprintf(name, sizeof(name), "%s/%s", spec.name, view.name);

                if (mode == Mode::Reference) {
                    // The view, then each step of its motion, each accumulated on its own.
                    for (int k = 0; k <= view.move_frames; ++k) {
                        place_camera(main_camera, inspect::motion_place(view, k));
                        for (long f = 0; f < reference_frames; ++f) {
                            inspect::place_movers(built, built.frames);
                            if (!frame()) return 1;
                        }
                        inspect::Image image;
                        if (!grab(image)) return 1;
                        char suffix[32];
                        std::snprintf(suffix, sizeof(suffix), k == 0 ? "" : "_move%d", k);
                        velk::string ref = reference_path(spec, view, suffix);
                        velk::string png = ref;
                        ref.append(".ref");
                        png.append(".png");
                        const bool saved = inspect::save_reference(inspect::downscale(image, kCompareScale), ref)
                                        && inspect::save_png(*writer, image, png);
                        VELK_LOG(I, "reference: %s%s %s", name, suffix, saved ? "saved" : "FAILED");
                    }
                    // Then what the scene moves, held at each frame of its movement that a run
                    // judges, from the view itself.
                    for (int k = 1; !built.movers.empty() && k <= kDuringSamples; ++k) {
                        const long at = during_frame(built, k);
                        place_camera(main_camera, view.place);
                        for (long f = 0; f < reference_frames; ++f) {
                            inspect::place_movers(built, at);
                            if (!frame()) return 1;
                        }
                        inspect::Image image;
                        if (!grab(image)) return 1;
                        char suffix[32];
                        std::snprintf(suffix, sizeof(suffix), "_at%ld", at);
                        velk::string ref = reference_path(spec, view, suffix);
                        velk::string png = ref;
                        ref.append(".ref");
                        png.append(".png");
                        const bool saved = inspect::save_reference(inspect::downscale(image, kCompareScale), ref)
                                        && inspect::save_png(*writer, image, png);
                        VELK_LOG(I, "reference: %s%s %s", name, suffix, saved ? "saved" : "FAILED");
                    }
                    continue;
                }

                // Settling, settle_frames after the load or after what the scene moves has
                // stopped: what changes from frame to frame meanwhile, and the frame after
                // which nothing more than kMaxChangeShare does. The reference has what moves where
                // it ends; a run sees it move there. On the way, frames of the movement are judged
                // against references with what moves held there.
                inspect::Image last;
                inspect::Image previous;
                float burst = 0.f;
                const long settle_from = std::max(kSettleFrom, built.movers.empty() ? 0l : built.frames);
                long settled = settle_from;
                // GPU time while the scene's movers move, after the first frames that load it.
                velk::vector<double> motion_ms;
                velk::vector<inspect::PassTime> motion_passes;
                inspect::Difference during{};
                long during_worst = 0;
                int during_judged = 0;
                bool during_references = true;
                int next_during = 1;
                inspect::Difference first{};
                bool have_first = false;
                for (long f = 0; f < settle_from + settle_frames; ++f) {
                    inspect::place_movers(built, f);
                    if (!frame()) return 1;
                    if (!built.movers.empty() && f >= kSettleFrom && f < built.frames) {
                        motion_ms.push_back(frame_ms(*backend));
                        add_passes(motion_passes);
                    }
                    if (!built.movers.empty() && next_during <= kDuringSamples && f == during_frame(built, next_during)) {
                        ++next_during;
                        inspect::Image image;
                        if (!grab(image)) return 1;
                        char suffix[32];
                        std::snprintf(suffix, sizeof(suffix), "_at%ld", f);
                        velk::string ours = run_path(spec, view, suffix);
                        ours.append(".png");
                        inspect::save_png(*writer, image, ours);
                        velk::string ref = reference_path(spec, view, suffix);
                        ref.append(".ref");
                        inspect::Image during_reference;
                        if (!inspect::load_reference(ref, during_reference)) {
                            during_references = false;
                        } else {
                            const inspect::Image small = inspect::downscale(image, kCompareScale);
                            const inspect::Difference d = inspect::compare(small, during_reference, tile);
                            velk::string diff = run_path(spec, view, suffix);
                            diff.append("_diff.png");
                            inspect::save_png(*writer, inspect::difference_map(small, during_reference), diff);
                            during.mean = std::max(during.mean, d.mean);
                            during.p95 = std::max(during.p95, d.p95);
                            if (d.worst_tile > during.worst_tile || during_judged == 0) {
                                during.worst_tile = d.worst_tile;
                                during.tile_x = d.tile_x;
                                during.tile_y = d.tile_y;
                                during_worst = f;
                            }
                            ++during_judged;
                        }
                    }
                    if (f < settle_from) continue;
                    previous = std::move(last);
                    if (!grab(last)) return 1;
                    // The first frame once loaded, or once what moves has stopped, before it
                    // settles: what a viewer sees first, against the reference.
                    if (f == settle_from) {
                        inspect::save_png(*writer, last, run_path(spec, view, "_first.png"));
                        inspect::Image first_reference;
                        if (inspect::load_reference(reference_path(spec, view, ".ref"), first_reference)) {
                            const inspect::Image small = inspect::downscale(last, kCompareScale);
                            first = inspect::compare(small, first_reference, tile);
                            inspect::save_png(*writer, inspect::difference_map(small, first_reference),
                                              run_path(spec, view, "_first_diff.png"));
                            have_first = true;
                        }
                    }
                    if (!previous.rgb.empty()) {
                        const float share = inspect::frame_change(previous, last, kChangeThreshold).share;
                        burst = std::max(burst, share);
                        if (share > kMaxChangeShare) settled = f + 1;
                    }
                }
                // Then still: frame to frame, the GPU time, each pass's, and memory.
                float changed = 0.f;
                double gpu_ms = 0.0;
                inspect::ViewTimes times;
                times.view = velk::string(name);
                for (long f = 0; f < kMeasureFrames; ++f) {
                    if (!frame()) return 1;
                    previous = std::move(last);
                    if (!grab(last)) return 1;
                    if (!previous.rgb.empty()) {
                        changed = std::max(changed, inspect::frame_change(previous, last, kChangeThreshold).share);
                    }
                    gpu_ms += frame_ms(*backend);
                    add_passes(times.passes);
                }
                gpu_ms /= static_cast<double>(kMeasureFrames);
                average_passes(times.passes, kMeasureFrames);
                times.settled_ms = gpu_ms;
                char motion_text[32];
                moving_text(motion_ms, times.moving_median_ms, times.moving_max_ms, motion_text, sizeof(motion_text));
                if (!motion_ms.empty()) {
                    average_passes(motion_passes, motion_ms.size());
                    times.moving_passes = std::move(motion_passes);
                }
                // Against the view's runs on committed code.
                inspect::ViewTimes base;
                velk::string base_since;
                const bool has_base = inspect::baseline_of(history, times.view, kBaselineRuns, base, base_since);
                char was_ms[16];
                was_text(has_base ? base.settled_ms : -1.0, was_ms, sizeof(was_ms));
                const bool still_slower = has_base && inspect::slower(gpu_ms, base.settled_ms);
                const bool moving_slower = has_base && base.moving_median_ms >= 0.0 && times.moving_median_ms >= 0.0
                                        && inspect::slower(times.moving_median_ms, base.moving_median_ms);
                // What the scene holds in the shared arenas, used bytes rather than their
                // buffers' size, which only grows; and the process's total, which grows with them.
                uint64_t arenas_used = 0;
                uint64_t cull_used = 0;
                for (uint32_t slot = 0; resources && slot < velk::IRenderBackend::kGlobalBufferSlotCount; ++slot) {
                    uint64_t capacity = 0;
                    uint64_t used = 0;
                    resources->arena_bytes(slot, capacity, used);
                    arenas_used += used;
                    if (slot == velk::IRenderBackend::kGlobalCulling) cull_used = used;
                }
                const double mb = static_cast<double>(backend->memory_in_use()) / 1e6;
                const double arenas_mb = static_cast<double>(arenas_used) / 1e6;
                const double cull_mb = static_cast<double>(cull_used) / 1e6;
                if (arenas_mb > peak_arenas_mb) {
                    peak_arenas_mb = arenas_mb;
                    std::snprintf(peak_at, sizeof(peak_at), "%s", name);
                }
                peak_mb = std::max(peak_mb, mb);
                peak_cull_mb = std::max(peak_cull_mb, cull_mb);

                const inspect::Image still = inspect::downscale(last, kCompareScale);
                inspect::save_png(*writer, last, run_path(spec, view, ".png"));
                inspect::Image reference;
                char settled_text[16];
                std::snprintf(settled_text, sizeof(settled_text), "%ld", settled);
                if (!inspect::load_reference(reference_path(spec, view, ".ref"), reference)) {
                    std::snprintf(buf, sizeof(buf), "%-24s %7s %7s %7s %8.5f %7s %8.5f %7.2f %7s %11s %7.0f %7.0f %7.0f  no reference",
                                  name, "-", "-", "-", static_cast<double>(burst), settled_text, static_cast<double>(changed), gpu_ms,
                                  was_ms, motion_text, mb, arenas_mb, cull_mb);
                    line(buf);
                    ++failed;
                } else {
                    const inspect::Difference d = inspect::compare(still, reference, tile);
                    inspect::save_png(*writer, inspect::difference_map(still, reference), run_path(spec, view, "_diff.png"));
                    char also[80];
                    std::snprintf(also, sizeof(also), "%s%s%s%s", burst > kMaxChangeShare ? " burst" : "",
                                  changed > kMaxChangeShare ? " flicker" : "", still_slower ? " slower" : "",
                                  moving_slower ? " slower moving" : "");
                    char verdict[160];
                    verdict_of(d, also, verdict, sizeof(verdict));
                    std::snprintf(buf, sizeof(buf), "%-24s %7.4f %7.4f %7.4f %8.5f %7s %8.5f %7.2f %7s %11s %7.0f %7.0f %7.0f  %s",
                                  name, static_cast<double>(d.mean), static_cast<double>(d.p95), static_cast<double>(d.worst_tile),
                                  static_cast<double>(burst), settled_text, static_cast<double>(changed), gpu_ms, was_ms,
                                  motion_text, mb, arenas_mb, cull_mb, verdict);
                    line(buf);
                    count(verdict);
                }
                if (still_slower) slower_passes("slower", times.passes, base.passes, base_since);
                if (moving_slower) slower_passes("slower moving", times.moving_passes, base.moving_passes, base_since);
                // The first frame, before settling.
                if (have_first) {
                    char first_name[64];
                    std::snprintf(first_name, sizeof(first_name), "%s first", name);
                    char verdict[160];
                    verdict_of(first, "", verdict, sizeof(verdict));
                    std::snprintf(buf, sizeof(buf), "%-24s %7.4f %7.4f %7.4f  at frame %ld, before settling  %s", first_name,
                                  static_cast<double>(first.mean), static_cast<double>(first.p95),
                                  static_cast<double>(first.worst_tile), settle_from, verdict);
                    line(buf);
                    count(verdict);
                }
                // The frames of the scene's movement, the worst of them.
                if (!built.movers.empty()) {
                    char during_name[64];
                    std::snprintf(during_name, sizeof(during_name), "%s during", name);
                    if (!during_references || during_judged == 0) {
                        std::snprintf(buf, sizeof(buf), "%-24s  no reference for every frame of the movement", during_name);
                        line(buf);
                        ++failed;
                    } else {
                        char verdict[160];
                        verdict_of(during, "", verdict, sizeof(verdict));
                        std::snprintf(buf, sizeof(buf), "%-24s %7.4f %7.4f %7.4f  worst at frame %ld of %ld, %d frames judged  %s",
                                      during_name, static_cast<double>(during.mean), static_cast<double>(during.p95),
                                      static_cast<double>(during.worst_tile), during_worst, built.frames, during_judged, verdict);
                        line(buf);
                        count(verdict);
                    }
                }
                if (view.move_frames <= 0) {
                    run_times.push_back(std::move(times));
                    continue;
                }

                // The motion: each step a frame, each against its own reference; the worst step.
                inspect::Difference moving{};
                int worst_step = 0;
                bool have_references = true;
                velk::vector<double> step_ms;
                velk::vector<inspect::PassTime> step_passes;
                for (int k = 1; k <= view.move_frames; ++k) {
                    place_camera(main_camera, inspect::motion_place(view, k));
                    if (!frame()) return 1;
                    step_ms.push_back(frame_ms(*backend));
                    add_passes(step_passes);
                    inspect::Image image;
                    if (!grab(image)) return 1;
                    char suffix[32];
                    std::snprintf(suffix, sizeof(suffix), "_move%d", k);
                    velk::string ref = reference_path(spec, view, suffix);
                    ref.append(".ref");
                    velk::string ours = run_path(spec, view, suffix);
                    ours.append(".png");
                    inspect::save_png(*writer, image, ours);
                    inspect::Image step_reference;
                    if (!inspect::load_reference(ref, step_reference)) {
                        have_references = false;
                        continue;
                    }
                    const inspect::Image small = inspect::downscale(image, kCompareScale);
                    const inspect::Difference d = inspect::compare(small, step_reference, tile);
                    velk::string diff = run_path(spec, view, suffix);
                    diff.append("_diff.png");
                    inspect::save_png(*writer, inspect::difference_map(small, step_reference), diff);
                    moving.mean = std::max(moving.mean, d.mean);
                    moving.p95 = std::max(moving.p95, d.p95);
                    if (d.worst_tile > moving.worst_tile) {
                        moving.worst_tile = d.worst_tile;
                        moving.tile_x = d.tile_x;
                        moving.tile_y = d.tile_y;
                        worst_step = k;
                    }
                }
                char moving_name[64];
                std::snprintf(moving_name, sizeof(moving_name), "%s moving", name);
                // The camera's motion's GPU time, against the view's runs on committed code.
                char step_text[32];
                moving_text(step_ms, times.moving_median_ms, times.moving_max_ms, step_text, sizeof(step_text));
                average_passes(step_passes, step_ms.size());
                times.moving_passes = std::move(step_passes);
                char step_was[16];
                was_text(has_base ? base.moving_median_ms : -1.0, step_was, sizeof(step_was));
                const bool steps_slower = has_base && base.moving_median_ms >= 0.0
                                       && inspect::slower(times.moving_median_ms, base.moving_median_ms);
                if (!have_references) {
                    std::snprintf(buf, sizeof(buf), "%-24s  no reference for every step", moving_name);
                    line(buf);
                    ++failed;
                } else {
                    char verdict[160];
                    verdict_of(moving, steps_slower ? " slower moving" : "", verdict, sizeof(verdict));
                    std::snprintf(buf, sizeof(buf), "%-24s %7.4f %7.4f %7.4f  worst at step %d of %d, ms %s (was %s)  %s", moving_name,
                                  static_cast<double>(moving.mean), static_cast<double>(moving.p95),
                                  static_cast<double>(moving.worst_tile), worst_step, view.move_frames, step_text, step_was, verdict);
                    line(buf);
                    count(verdict);
                }
                if (steps_slower) slower_passes("slower moving", times.moving_passes, base.moving_passes, base_since);
                run_times.push_back(std::move(times));

                // There and back: the view again after the motion, against how it looked before.
                place_camera(main_camera, view.place);
                for (long f = 0; f < kBackFrames; ++f) {
                    if (!frame()) return 1;
                }
                inspect::Image back;
                if (!grab(back)) return 1;
                inspect::save_png(*writer, back, run_path(spec, view, "_back.png"));
                const inspect::Difference d = inspect::compare(inspect::downscale(back, kCompareScale), still, tile);
                char verdict[160];
                verdict_of(d, "", verdict, sizeof(verdict));
                char back_name[64];
                std::snprintf(back_name, sizeof(back_name), "%s back", name);
                std::snprintf(buf, sizeof(buf), "%-24s %7.4f %7.4f %7.4f  against before the motion  %s", back_name,
                              static_cast<double>(d.mean), static_cast<double>(d.p95), static_cast<double>(d.worst_tile), verdict);
                line(buf);
                count(verdict);
            }

            if (mode == Mode::Verify) {
                std::snprintf(buf, sizeof(buf),
                              "memory: arenas in use at most %.0f MB (%s), the culling arena's at most %.0f MB; the process held "
                              "at most %.0f MB (arenas give none back)",
                              peak_arenas_mb, peak_at, peak_cull_mb, peak_mb);
                line(buf);
                std::snprintf(buf, sizeof(buf), "%zu passed, %zu failed; images in %s", passed, failed, INSPECT_RUN_DIR);
                line(buf);
                // A run of every scene goes into the history, with the commits it ran.
                if (!only_scene && !skip_scenes) {
                    bool dirty = false;
                    bool velk_dirty = false;
                    const velk::string commit = inspect::commit_of(view_of(INSPECT_SOURCE_DIR), view_of(INSPECT_MEASURED_PATHS), dirty);
                    const velk::string velk_commit =
                        inspect::commit_of(view_of(INSPECT_VELK_DIR), view_of(INSPECT_VELK_MEASURED_PATHS), velk_dirty);
                    char date[32] = "";
                    const std::time_t now = std::time(nullptr);
                    if (const std::tm* local = std::localtime(&now)) std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", local);
                    if (inspect::append_timings(view_of(kTimingsPath), velk::string(date), commit, velk_commit, dirty || velk_dirty,
                                                run_times)) {
                        std::snprintf(buf, sizeof(buf), "times added to the history at %s%s %s%s", commit.c_str(), dirty ? "*" : "",
                                      velk_commit.c_str(), velk_dirty ? "*" : "");
                        line(buf);
                    }
                }
                char path[512];
                std::snprintf(path, sizeof(path), "%s/report.txt", INSPECT_RUN_DIR);
                if (std::FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(report.c_str(), 1, report.size(), f);
                    std::fclose(f);
                }
            }
            return 0;
        }();
    }

    // Interactive: the chosen scene at its first view, unless at= says where.
    const size_t interactive_index = static_cast<size_t>(interactive_spec - specs.data());
    show_scene(interactive_index);
    const inspect::Place start = has_place ? inspect::Place{{place[0], place[1], place[2]}, place[3], place[4]}
                                           : interactive_spec->views[0].place;
    place_camera(main_camera, start);

    // The overview hangs above the main camera, looking down, and draws what the main camera draws.
    FlyCamera overview = add_camera(scene, root, false);
    overview.camera.set_choose_as(main_camera.element);
    constexpr float kOverviewHeight = 2500.f;
    overview.orbit.set_pitch(-89.f);
    app.add_view(window, overview.element, {0.66f, 0.f, 0.34f, 0.4f});

    // What is shown and what its colours mean, in a 2D scene of its own drawn last, over the
    // bottom left of the window.
    auto overlay_scene = velk::create_scene();
    overlay_scene.set_geometry(velk::aabb::from_size({static_cast<float>(wc.width), static_cast<float>(wc.height)}));
    auto overlay_root = velk::create_element();
    overlay_scene.set_root(overlay_root);
    auto overlay_camera = velk::create_element();
    overlay_scene.add(overlay_root, overlay_camera);
    auto overlay_camera_trait = velk::trait::render::create_camera();
    overlay_camera_trait.set_projection(velk::Projection::Ortho);
    overlay_camera.add_trait(overlay_camera_trait);
    constexpr float kTextWidth = 760.f;
    constexpr float kTextHeight = 112.f;
    auto text_element = velk::create_element();
    auto text_trs = velk::trait::transform::create_trs();
    text_trs.set_translate({8.f, static_cast<float>(wc.height) - kTextHeight - 8.f, 0.f});
    text_element.add_trait(text_trs);
    text_element.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(kTextWidth), velk::dim::px(kTextHeight)));
    auto text_background = velk::ui::trait::visual::create_rect();
    text_background.set_color({0.f, 0.f, 0.f, 0.55f});
    text_element.add_trait(text_background);
    auto text = velk::ui::trait::visual::create_text();
    text.set_font(velk::ui::get_default_font());
    text.set_font_size(14.f);
    text.set_color({1.f, 1.f, 0.f, 1.f});
    text.set_layout(velk::TextLayout::MultiLine);
    text_element.add_trait(text);
    overlay_scene.add(overlay_root, text_element);
    app.add_view(window, overlay_camera);

    size_t view_index = 0;
    bool scripted = start_scripted;
    bool text_stale = true;
    velk::vec3 shown_at{};
    float shown_yaw = 0.f;
    float shown_pitch = 0.f;
    const auto show_text = [&](const velk::vec3& at, float yaw, float pitch) {
        char level[32] = "";
        if (kViews[view_index].view == velk::DebugView::RayTree || kViews[view_index].view == velk::DebugView::RayTop) {
            std::snprintf(level, sizeof(level), ", level %u", tree_level);
        }
        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "scene: %s, view: %s%s (V for next)\n%s\n"
                      "top right, from above: all the main camera's kept cut holds, dimmed outside its view (yellow lines)\n"
                      "camera: %s (P), at %.0f, %.0f, %.0f, yaw %.0f, pitch %.0f\n"
                      "right drag turns, W A S D move, wheel moves along the view, C logs the place",
                      interactive_spec->name, kViews[view_index].name, level, kViews[view_index].legend,
                      scripted ? "scripted path" : "free", at.x, at.y, at.z, yaw, pitch);
        text.set_text(velk::string(buf));
        shown_at = at;
        shown_yaw = yaw;
        shown_pitch = pitch;
    };
    const auto show_view = [&](size_t index) {
        view_index = index % kViewCount;
        text_stale = true;
        main_camera.camera.set_debug_view(kViews[view_index].view);
        overview.camera.set_debug_view(kViews[view_index].view);
        VELK_LOG(I, "inspect: view %s", kViews[view_index].name);
    };
    const auto show_level = [&](uint32_t level) {
        tree_level = level;
        text_stale = true;
        main_camera.camera.set_debug_level(level);
        overview.camera.set_debug_level(level);
    };

    long scripted_frame = start_frame;
    bool turning = false;
    float last_x = 0.f;
    float last_y = 0.f;
    bool held[4] = {false, false, false, false};

    const auto camera_state = [&]() {
        auto orbit_state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(main_camera.orbit));
        auto focus_state = velk::read_state<velk::ITrs>(static_cast<velk::ITrs::Ptr>(main_camera.focus));
        return std::make_pair(orbit_state, focus_state);
    };
    const auto move = [&](float forward, float right) {
        auto [orbit_state, focus_state] = camera_state();
        if (!orbit_state || !focus_state) {
            return;
        }
        // Faster the higher above the ground.
        const float speed = std::max(60.f, -focus_state->translate.y);
        const velk::vec3 ahead = inspect::forward_of(orbit_state->yaw, orbit_state->pitch);
        const float yaw = orbit_state->yaw * kPi / 180.f;
        const velk::vec3 side{std::cos(yaw), 0.f, -std::sin(yaw)};
        main_camera.focus.set_translate(focus_state->translate + ahead * (forward * speed) + side * (right * speed));
    };

    velk::ScopedHandler pointer_sub = window.add_on_pointer_event([&](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down && e.button == velk::ui::PointerButton::Right) {
            turning = true;
            last_x = e.position.x;
            last_y = e.position.y;
        } else if (e.action == velk::ui::PointerAction::Up && e.button == velk::ui::PointerButton::Right) {
            turning = false;
        } else if (e.action == velk::ui::PointerAction::Move && turning && !scripted) {
            auto [orbit_state, focus_state] = camera_state();
            if (orbit_state) {
                main_camera.orbit.set_yaw(orbit_state->yaw - (e.position.x - last_x) * 0.2f);
                main_camera.orbit.set_pitch(std::clamp(orbit_state->pitch - (e.position.y - last_y) * 0.2f, -89.f, 89.f));
            }
            last_x = e.position.x;
            last_y = e.position.y;
        }
    });
    velk::ScopedHandler scroll_sub(window.input()->on_scroll_event(), [&](const velk::ui::ScrollEvent& e) {
        if (!scripted) {
            move(e.delta.y > 0 ? 0.5f : -0.5f, 0.f);
        }
    });
    velk::ScopedHandler key_sub = window.add_on_key_event([&](const velk::ui::KeyEvent& e) {
        const bool down = e.action != velk::ui::KeyAction::Up;
        // W, S, A and D.
        if (e.key == 87) held[0] = down;
        if (e.key == 83) held[1] = down;
        if (e.key == 65) held[2] = down;
        if (e.key == 68) held[3] = down;
        if (e.action != velk::ui::KeyAction::Down) {
            return;
        }
        if (e.key == 86) {  // V
            show_view(view_index + 1);
        } else if (e.key == 76) {  // L
            show_level(tree_level + 1);
        } else if (e.key == 75) {  // K
            show_level(tree_level > 0 ? tree_level - 1 : 0);
        } else if (e.key == 80) {  // P
            scripted = !scripted;
            scripted_frame = 0;
            text_stale = true;
            VELK_LOG(I, "inspect: scripted path %s", scripted ? "on" : "off");
        } else if (e.key == 67) {  // C
            auto [orbit_state, focus_state] = camera_state();
            if (orbit_state && focus_state) {
                VELK_LOG(I, "inspect: at=%.1f,%.1f,%.1f,%.1f,%.1f", focus_state->translate.x, focus_state->translate.y,
                         focus_state->translate.z, orbit_state->yaw, orbit_state->pitch);
            }
        }
    });

    show_view(start_view);
    show_level(tree_level);

    // Frames much slower than those before them, logged with where their GPU time went: the
    // frame's time between presents, and the GPU time of the last frame it has times for.
    auto frame_backend = render_context->backend();
    if (frame_backend) frame_backend->set_gpu_timing_enabled(true);
    constexpr size_t kSlowWindow = 120;
    velk::vector<double> recent_wall;
    velk::vector<double> recent_gpu;
    size_t recent_at = 0;
    auto last_present = std::chrono::steady_clock::now();
    const auto median_of = [](velk::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[values.size() / 2];
    };
    const auto check_slow = [&](long at) {
        const auto now = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double, std::milli>(now - last_present).count();
        last_present = now;
        const double gpu = frame_backend ? frame_ms(*frame_backend) : 0.0;
        if (recent_wall.size() >= 30) {
            const double wall_median = median_of(recent_wall);
            const double gpu_median = median_of(recent_gpu);
            const bool wall_slow = wall > 2.0 * wall_median && wall > wall_median + 4.0;
            const bool gpu_slow = gpu > 2.0 * gpu_median && gpu > gpu_median + 2.0;
            if (wall_slow || gpu_slow) {
                struct Pass
                {
                    const char* name;
                    double ms;
                };
                velk::vector<Pass> passes;
                if (frame_backend) {
                    for (const auto& t : frame_backend->last_gpu_timings()) passes.push_back({t.label, double(t.ms)});
                }
                std::sort(passes.begin(), passes.end(), [](const Pass& a, const Pass& b) { return a.ms > b.ms; });
                char top[256] = "";
                size_t used = 0;
                for (size_t i = 0; i < passes.size() && i < 4u && used < sizeof(top); ++i) {
                    used += static_cast<size_t>(std::snprintf(top + used, sizeof(top) - used, "%s%s %.2f", i == 0 ? "" : ", ",
                                                              passes[i].name, passes[i].ms));
                }
                VELK_LOG(I, "inspect: slow frame %ld: %.1f ms (median %.1f), GPU %.2f ms (median %.2f): %s", at, wall,
                         wall_median, gpu, gpu_median, top);
            }
        }
        if (recent_wall.size() < kSlowWindow) {
            recent_wall.push_back(wall);
            recent_gpu.push_back(gpu);
        } else {
            recent_wall[recent_at] = wall;
            recent_gpu[recent_at] = gpu;
            recent_at = (recent_at + 1u) % kSlowWindow;
        }
    };

    long frame = 0;
    while (app.poll()) {
        // What the scene moves goes there and back, again and again.
        const long cycle = std::max(built.frames, 1l) * 2;
        const long at = frame % cycle;
        inspect::place_movers(built, at <= built.frames ? at : cycle - at);
        ++frame;
        if (scripted) {
            // Out along the view and back, rising and falling, the same every time, from the start.
            const float t = static_cast<float>(scripted_frame++) / 60.f;
            const float along = 0.5f - 0.5f * std::cos(t * 0.15f);
            main_camera.focus.set_translate({start.at.x + std::sin(t * 0.21f) * 250.f, start.at.y - 400.f * along * along,
                                             start.at.z - 3800.f * along});
            main_camera.orbit.set_yaw(start.yaw + std::sin(t * 0.33f) * 40.f);
            main_camera.orbit.set_pitch(start.pitch - 12.f * along);
        } else {
            constexpr float kStepSeconds = 1.f / 60.f;
            move(((held[0] ? 1.f : 0.f) - (held[1] ? 1.f : 0.f)) * kStepSeconds,
                 ((held[3] ? 1.f : 0.f) - (held[2] ? 1.f : 0.f)) * kStepSeconds);
        }
        // The overview follows the main camera from above, turned the same way.
        auto [orbit_state, focus_state] = camera_state();
        if (orbit_state && focus_state) {
            overview.focus.set_translate(focus_state->translate + velk::vec3{0.f, -kOverviewHeight, 0.f});
            overview.orbit.set_yaw(orbit_state->yaw);
            // The text again when the view or mode changed, or the camera moved or turned a little.
            const velk::vec3 moved = focus_state->translate - shown_at;
            if (text_stale || std::abs(moved.x) + std::abs(moved.y) + std::abs(moved.z) > 5.f
                || std::abs(orbit_state->yaw - shown_yaw) > 1.f || std::abs(orbit_state->pitch - shown_pitch) > 1.f) {
                show_text(focus_state->translate, orbit_state->yaw, orbit_state->pitch);
                text_stale = false;
            }
        }
        app.update();
        app.present();
        check_slow(frame);
    }
    return 0;
}
