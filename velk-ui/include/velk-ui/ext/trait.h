#ifndef VELK_UI_EXT_TRAIT_H
#define VELK_UI_EXT_TRAIT_H

#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-ui/interface/intf_camera.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-ui/interface/intf_transform_trait.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk::ui::ext {

/**
 * @brief CRTP base for ILayoutTrait implementations.
 *
 * Provides default no-op measure/apply and a compile-time phase.
 * Subclasses override only what they need.
 *
 * @tparam T     The concrete trait class (CRTP parameter).
 * @tparam Phase The layout phase (Layout or Constraint).
 * @tparam Extra Additional interfaces the trait implements (e.g. IStack, IFixedSize).
 */
template <class T, TraitPhase Phase, class... Extra>
class Layout : public ::velk::ext::Object<T, ILayoutTrait, Extra...>
{
public:
    TraitPhase get_phase() const override { return Phase; }
    Constraint measure(const Constraint& c, IElement&, IHierarchy&) override { return c; }
    void apply(const Constraint&, IElement&, IHierarchy&) override {}
};

/**
 * @brief CRTP base for ITransformTrait implementations.
 *
 * @tparam T     The concrete trait class (CRTP parameter).
 * @tparam Extra Additional interfaces the trait implements (e.g. ITrs, IMatrix).
 */
template <class T, class... Extra>
class Transform : public ::velk::ext::Object<T, ITransformTrait, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Transform; }
    void transform(IElement&) override {}
};

/**
 * @brief CRTP base for IVisual implementations.
 *
 * Bakes in IVisual and IMetadataObserver. Provides invoke_visual_changed()
 * and a default on_state_changed that fires the event automatically.
 *
 * @tparam T     The concrete visual class (CRTP parameter).
 * @tparam Extra Additional interfaces the visual implements (e.g. ITextureProvider).
 */
template <class T, class... Extra>
class Visual : public ::velk::ext::Object<T, IVisual, IMetadataObserver, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Visual; }

protected:
    /** @brief Fires the on_visual_changed event. Call from subclasses when visual state changes. */
    void invoke_visual_changed()
    {
        invoke_event(this->get_interface(IInterface::UID), "on_visual_changed");
    }

    /** @brief Default: any property change fires on_visual_changed. Override to filter. */
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override
    {
        invoke_visual_changed();
    }
};

/**
 * @brief CRTP base for ICamera implementations.
 *
 * @tparam T     The concrete camera class (CRTP parameter).
 * @tparam Extra Additional interfaces.
 */
template <class T, class... Extra>
class Camera : public ::velk::ext::Object<T, ICamera, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Render; }
};

} // namespace velk::ui::ext

#endif // VELK_UI_EXT_TRAIT_H
