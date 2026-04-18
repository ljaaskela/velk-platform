#ifndef VELK_UI_API_RENDER_TRAIT_H
#define VELK_UI_API_RENDER_TRAIT_H

#include <velk/api/object.h>

#include <velk-render/api/render_technique.h>
#include <velk-render/interface/intf_render_trait.h>

namespace velk::ui {

/**
 * @brief Base wrapper for `IRenderTrait` attachments (Camera, Light,
 *        and future render-trait kinds).
 *
 * Peer to `Trait` (which wraps `ITrait`). Both attach to an Element
 * via `Element::add_trait`; the Element surface has distinct
 * overloads so the correct interface contract is enforced.
 *
 * RenderTrait hosts render-technique attachments scoped to this
 * particular trait (shadow on a light, reflection/AO/GI/post on a
 * camera).
 *
 *   auto light = trait::render::create_directional_light(...);
 *   light.add_technique(create_rt_shadow());
 *   auto sun = ui::create_element();
 *   sun.add_trait(light);
 */
class RenderTrait : public Object
{
public:
    RenderTrait() = default;

    explicit RenderTrait(IObject::Ptr obj)
        : Object(check_object<IRenderTrait>(obj)) {}

    explicit RenderTrait(IRenderTrait::Ptr t)
        : Object(as_object(t)) {}

    /** @brief Implicit conversion to IRenderTrait::Ptr (for Element::add_trait dispatch). */
    operator IRenderTrait::Ptr() const { return as_ptr<IRenderTrait>(); }

    /**
     * @brief Attaches a render technique scoped to this trait.
     * @return Success on success, InvalidArgument if `tech` is empty.
     */
    ReturnValue add_technique(const RenderTechnique& tech)
    {
        return tech ? add_attachment(static_cast<IRenderTechnique::Ptr>(tech))
                    : ReturnValue::InvalidArgument;
    }

    /** @brief Removes a previously attached technique from this trait. */
    ReturnValue remove_technique(const RenderTechnique& tech)
    {
        return tech ? remove_attachment(static_cast<IRenderTechnique::Ptr>(tech))
                    : ReturnValue::InvalidArgument;
    }
};

} // namespace velk::ui

#endif // VELK_UI_API_RENDER_TRAIT_H
