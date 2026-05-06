#ifndef VELK_SCENE_API_POST_PROCESS_H
#define VELK_SCENE_API_POST_PROCESS_H

#include <velk/api/object.h>

#include <velk-render/interface/intf_effect.h>
#include <velk-render/interface/intf_post_process.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Typed wrapper around an `IEffect` object.
 *
 * Effects are leaf computation nodes: shader with predefined inputs
 * and outputs. Attached to an `IPostProcess` container, which orders
 * them and supplies intermediate textures.
 */
class Effect : public Object
{
public:
    Effect() = default;

    explicit Effect(IObject::Ptr obj)
        : Object(check_object<IEffect>(obj)) {}

    explicit Effect(IEffect::Ptr p)
        : Object(as_object(p)) {}

    operator IEffect::Ptr() const { return as_ptr<IEffect>(); }
};

/**
 * @brief Typed wrapper around an `IPostProcess` object.
 *
 * The camera-level post-process container. Holds attached `IEffect`
 * children and orchestrates them when the view pipeline emits its
 * frame. Each camera that wants post-processing attaches one
 * `PostProcess` to its view pipeline (or via
 * `Camera::add_post_process` for the default pipeline).
 *
 *   auto post = pp::create_post_process();
 *   post.add(pp::create_tonemap());
 *   camera.add_post_process(post);
 *
 * One `PostProcess` Ptr can attach to multiple cameras / pipelines;
 * per-view state (intermediates) keys off `IViewEntry*` so views
 * stay isolated.
 */
class PostProcess : public Object
{
public:
    PostProcess() = default;

    explicit PostProcess(IObject::Ptr obj)
        : Object(check_object<IPostProcess>(obj)) {}

    explicit PostProcess(IPostProcess::Ptr p)
        : Object(as_object(p)) {}

    operator IPostProcess::Ptr() const { return as_ptr<IPostProcess>(); }

    /// Attaches @p effect as a child of this post-process. Children
    /// run in attachment order; the last child writes to the
    /// post-process's output, others to internal intermediates.
    ReturnValue add(const Effect& effect)
    {
        return add_attachment(static_cast<IEffect::Ptr>(effect));
    }

    /// Removes a previously attached effect.
    ReturnValue remove(const Effect& effect)
    {
        return remove_attachment(static_cast<IEffect::Ptr>(effect));
    }
};

namespace pp {

/// Creates the default linear post-process container. Add effects
/// with `post.add(pp::create_tonemap())` etc.
inline PostProcess create_post_process()
{
    return PostProcess(instance().create<IObject>(ClassId::Post::PostProcess));
}

/// ACES filmic tonemap effect.
inline Effect create_tonemap()
{
    return Effect(instance().create<IObject>(ClassId::Effect::Tonemap));
}

} // namespace pp

} // namespace velk

#endif // VELK_SCENE_API_POST_PROCESS_H
