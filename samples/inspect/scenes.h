#ifndef VELK_INSPECT_SCENES_H
#define VELK_INSPECT_SCENES_H

#include <velk/vector.h>
#include <velk-render/api/render_context.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/trait/trs.h>

namespace inspect {

/// Where the camera is and which way it looks, in degrees. Up is -y.
struct Place
{
    velk::vec3 at{};
    float yaw = 0.f;
    float pitch = 0.f;
};

/// A fixed view of a scene, by name, and a motion from it: `move_frames` frames to `move_to`,
/// a step a frame, for checking each frame of a camera move against its own reference. No
/// motion when `move_frames` is 0.
struct ViewSpec
{
    const char* name;
    Place place;
    Place move_to{};
    int move_frames = 0;
    /// In a scene with a look switch, whether the view is at night.
    bool night = false;
    /// Another view of the scene whose reference this one is judged against, which a reference
    /// run then makes no reference of its own for: the same place and look, reached otherwise.
    const char* reference = nullptr;
    /// Frames the camera stays at `move_to` after its motion before it comes back; with any, the
    /// view is also judged at the kSettleFrom-th frame back, before it settles again: what is seen
    /// on turning back to what was out of view that long.
    int away_frames = 0;
};

/// Where the camera is at step @p k of @p view's motion, 0 its start.
Place motion_place(const ViewSpec& view, int k);

/// Something a scene moves: its transform, from `from` to `to`.
struct Mover
{
    velk::Trs trs;
    velk::vec3 from{};
    velk::vec3 to{};
};

/// A light a scene's look switch sets: its intensity by day and by night.
struct LookLight
{
    velk::Light light;
    float day = 0.f;
    float night = 0.f;
};

/// What a scene built: its elements, each a child of the scene's root (shapes take their place
/// from layout, so they stay at the top), and what it moves over its first `frames` frames. A
/// scene with a look switch: its lights, and the frames from `night_from` to `night_to` it is
/// night in a run, day before and after.
struct BuiltScene
{
    velk::vector<velk::Element> elements;
    velk::vector<Mover> movers;
    long frames = 0;
    velk::vector<LookLight> look;
    long night_from = 0;
    long night_to = 0;
    int night = -1;  ///< The look its lights have: 1 night, 0 day, -1 not set yet.
};

/// Places each of @p built's movers where it is at frame @p frame: on its way, then at its end.
void place_movers(BuiltScene& built, long frame);

/// Sets @p built's look switch lights to night or day, when they are not that already.
void set_look(BuiltScene& built, bool night);

/// What a scene builder gets: the scene, its root to build under, the render context, and the
/// camera, whose environment the scene sets.
struct SceneBuilder
{
    velk::Scene& scene;
    velk::Element& root;
    velk::RenderContext& ctx;
    velk::Camera& camera;
    BuiltScene& out;
};

/// A scene of the verification set: its name, what it tests, its views, its builder, and, when
/// its reference is to be made of something else that should look the same, the builder of that.
struct SceneSpec
{
    const char* name;
    const char* about;
    velk::vector<ViewSpec> views;
    void (*build)(SceneBuilder&);
    void (*build_reference)(SceneBuilder&) = nullptr;
    /// Whether verify and reference run it without scene= naming it: false for a scene too
    /// heavy for every run, kept for looking into one problem.
    bool in_set = true;
};

/// The verification scenes, simplest first.
const velk::vector<SceneSpec>& scenes();

/// The scene called @p name, or null.
const SceneSpec* find_scene(const char* name);

/// Builds @p spec under @p scene's root, as its reference is made with @p reference, and sets
/// @p camera's environment for it.
BuiltScene build_scene(const SceneSpec& spec, velk::Scene& scene, velk::Element& root, velk::RenderContext& ctx,
                       velk::Camera& camera, bool reference = false);

/// Where a camera at yaw and pitch in degrees looks.
velk::vec3 forward_of(float yaw_degrees, float pitch_degrees);

} // namespace inspect

#endif // VELK_INSPECT_SCENES_H
