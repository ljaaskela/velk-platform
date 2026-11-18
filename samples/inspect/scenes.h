#ifndef VELK_INSPECT_SCENES_H
#define VELK_INSPECT_SCENES_H

#include <velk/vector.h>
#include <velk-render/api/render_context.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
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
};

/// Where the camera is at step @p k of @p view's motion, 0 its start.
Place motion_place(const ViewSpec& view, int k);

/// What a scene built: its elements, each a child of the scene's root (shapes take their place
/// from layout, so they stay at the top), and an object it moves from `from` to `to` over its
/// first `frames` frames, if it has one.
struct BuiltScene
{
    velk::vector<velk::Element> elements;
    bool moves = false;
    velk::Trs mover;
    velk::vec3 from{};
    velk::vec3 to{};
    long frames = 0;
};

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
};

/// The verification scenes, simplest first.
const velk::vector<SceneSpec>& scenes();

/// The scene called @p name, or null.
const SceneSpec* find_scene(const char* name);

/// Builds @p spec under @p scene's root, as its reference is made with @p reference, and sets
/// @p camera's environment for it.
BuiltScene build_scene(const SceneSpec& spec, velk::Scene& scene, velk::Element& root, velk::RenderContext& ctx,
                       velk::Camera& camera, bool reference = false);

/// Takes what @p built added out of @p scene again.
void remove_scene(BuiltScene& built, velk::Scene& scene);

/// Where a camera at yaw and pitch in degrees looks.
velk::vec3 forward_of(float yaw_degrees, float pitch_degrees);

} // namespace inspect

#endif // VELK_INSPECT_SCENES_H
