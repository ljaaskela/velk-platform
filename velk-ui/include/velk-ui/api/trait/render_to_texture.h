#ifndef VELK_UI_API_TRAIT_RENDER_CACHE_H
#define VELK_UI_API_TRAIT_RENDER_CACHE_H

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-scene/api/element.h>
#include <velk-scene/interface/intf_render_to_texture.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper for a RenderCache trait.
 *
 * Caches the element's rendered subtree into a RenderTexture.
 * The texture can be displayed via a TextureVisual on another element.
 *
 *   auto rc = trait::render::create_render_cache();
 *   element.add_trait(rc);
 */
class RenderCacheTrait : public Trait
{
public:
    RenderCacheTrait() = default;
    explicit RenderCacheTrait(IObject::Ptr obj) : Trait(std::move(obj)) {}

    /** @brief Sets the texture resolution. (0,0) matches the element's layout size. */
    void set_texture_size(uint32_t w, uint32_t h)
    {
        write_state<IRenderToTexture>(ptr(), [&](IRenderToTexture::State& s) {
            s.texture_size = {w, h};
        });
    }

    /** @brief Returns the render target (RenderTexture) for binding to a TextureVisual. */
    IObject::Ptr get_render_target() const
    {
        auto state = read_state<IRenderToTexture>(ptr());
        return state ? state->render_target.get() : nullptr;
    }
};

namespace trait::render {

/** @brief Creates a new RenderCache trait. */
inline RenderCacheTrait create_render_cache()
{
    return RenderCacheTrait(instance().create<IObject>(ClassId::Render::RenderCache));
}

} // namespace trait::render

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_RENDER_CACHE_H
