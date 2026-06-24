#ifndef VELK_SCENE_API_POST_PROCESS_H
#define VELK_SCENE_API_POST_PROCESS_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_bloom.h>
#include <velk-render/interface/intf_effect.h>
#include <velk-render/interface/intf_post_process.h>
#include <velk-render/interface/intf_tonemap.h>
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
 * @brief Typed wrapper around the tonemap effect, adding exposure control.
 *
 * The tonemap object also implements `ITonemap`, whose `exposure` property
 * scales HDR radiance before the ACES curve. Drive it at runtime to lift or
 * darken a scene (e.g. a brighter exposure for a dim night look).
 */
class Tonemap : public Effect
{
public:
    Tonemap() = default;
    explicit Tonemap(IObject::Ptr obj) : Effect(obj) {}
    explicit Tonemap(IEffect::Ptr p) : Effect(p) {}

    /// Sets the exposure multiplier (1 = neutral, > 1 brighter).
    void set_exposure(float v)
    {
        write_state_value<ITonemap>(&ITonemap::State::exposure, v);
    }

    /// Returns the exposure multiplier.
    auto get_exposure() const
    {
        return read_state_value<ITonemap>(&ITonemap::State::exposure);
    }
};

/**
 * @brief Typed wrapper around the bloom effect.
 *
 * Bloom blurs the bright (HDR) part of the image across a mip chain and adds
 * it back as a soft glow. Place it before the tonemap in the post chain (it
 * operates on HDR radiance). Drive the look at runtime via the setters, e.g.
 * raise `intensity` for a night scene and drop it to 0 for daytime.
 */
class Bloom : public Effect
{
public:
    Bloom() = default;
    explicit Bloom(IObject::Ptr obj) : Effect(obj) {}
    explicit Bloom(IEffect::Ptr p) : Effect(p) {}

    /// Luminance below which a pixel does not bloom.
    void set_threshold(float v) { write_state_value<IBloom>(&IBloom::State::threshold, v); }
    /// Soft-knee width around the threshold.
    void set_knee(float v) { write_state_value<IBloom>(&IBloom::State::knee, v); }
    /// Strength the glow is added back at (0 = bloom off).
    void set_intensity(float v) { write_state_value<IBloom>(&IBloom::State::intensity, v); }
    /// Upsample tent-filter scale; larger spreads the glow wider.
    void set_radius(float v) { write_state_value<IBloom>(&IBloom::State::radius, v); }

    auto get_intensity() const { return read_state_value<IBloom>(&IBloom::State::intensity); }
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

/// ACES filmic tonemap effect. @p exposure scales HDR radiance before the
/// curve (1 = neutral); adjust later via `Tonemap::set_exposure`.
inline Tonemap create_tonemap(float exposure = 1.f)
{
    Tonemap t(instance().create<IObject>(ClassId::Effect::Tonemap));
    if (t) t.set_exposure(exposure);
    return t;
}

/// Progressive mip-chain bloom effect. Add it before the tonemap. @p intensity
/// is the glow strength (0 = off); tune all knobs later via the setters.
inline Bloom create_bloom(float intensity = 0.05f, float threshold = 1.f)
{
    Bloom b(instance().create<IObject>(ClassId::Effect::Bloom));
    if (b) {
        b.set_intensity(intensity);
        b.set_threshold(threshold);
    }
    return b;
}

} // namespace pp

} // namespace velk

#endif // VELK_SCENE_API_POST_PROCESS_H
