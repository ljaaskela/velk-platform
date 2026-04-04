#ifndef VELK_UI_INPUT_DISPATCHER_H
#define VELK_UI_INPUT_DISPATCHER_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugin.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-ui/interface/intf_scene.h>

namespace velk::ui::impl {

class InputDispatcher : public ::velk::ext::Object<InputDispatcher, IInputDispatcher>
{
public:
    VELK_CLASS_UID(ClassId::Input::Dispatcher, "InputDispatcher");

    // IInputDispatcher
    void set_scene(const shared_ptr<IScene>& scene) override;
    void pointer_event(const PointerEvent& event) override;
    void scroll_event(const ScrollEvent& event) override;
    void key_event(const KeyEvent& event) override;

    IElement::Ptr get_hovered() const override;
    IElement::Ptr get_pressed() const override;
    IElement::Ptr get_focused() const override;
    void set_focus(const IElement::Ptr& element) override;

private:
    /** @brief Finds the deepest element with an IInputTrait under the given point. */
    IElement* hit_test(vec2 point) const;

    /** @brief Returns the IInputTrait attached to an element, or nullptr. */
    static IInputTrait* get_input_trait(IElement* element);

    /** @brief Computes the element's world-space AABB from world_matrix + size. */
    static rect get_world_rect(IElement* element);

    /** @brief Converts a scene-space point to element-local space. */
    static vec2 to_local(IElement* element, vec2 scene_point);

    /**
     * @brief Builds the ancestor chain from root to the target element.
     *
     * Used for intercept (top-down) and bubble (bottom-up) passes.
     * Only includes ancestors that have an IInputTrait.
     */
    void build_ancestor_chain(IElement* target, vector<IElement*>& chain) const;

    /** @brief Dispatches a pointer event through the intercept + bubble pipeline. */
    InputResult dispatch_pointer(PointerEvent& event, IElement* hit);

    /** @brief Dispatches a scroll event through the intercept + bubble pipeline. */
    InputResult dispatch_scroll(ScrollEvent& event, IElement* hit);

    /** @brief Updates hover state and fires enter/leave. */
    void update_hover(IElement* new_hover, const PointerEvent& event);

    weak_ptr<IScene> scene_;
    IElement::Ptr hovered_;
    IElement::Ptr pressed_;
    IElement::Ptr focused_;
    bool captured_ = false;
};

} // namespace velk::ui::impl

#endif // VELK_UI_INPUT_DISPATCHER_H
