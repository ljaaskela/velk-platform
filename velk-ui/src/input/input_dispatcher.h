#ifndef VELK_UI_INPUT_DISPATCHER_H
#define VELK_UI_INPUT_DISPATCHER_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugin.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-scene/interface/intf_scene.h>

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
    /** @brief Finds the topmost element with an IInputTrait under the given point. */
    IElement::Ptr hit_test(vec2 point) const;

    /** @brief Converts a scene-space point to element-local space. */
    static vec2 to_local(const IElement::Ptr& element, vec2 scene_point);

    /**
     * @brief Builds the ancestor chain from root to the target element.
     *
     * Used for intercept (top-down) and bubble (bottom-up) passes.
     * Only includes ancestors that have an IInputTrait.
     */
    void build_ancestor_chain(const IElement::Ptr& target, vector<IElement::Ptr>& chain) const;

    /** @brief Dispatches a pointer event through the intercept + bubble pipeline. */
    InputResult dispatch_pointer(PointerEvent& event, const IElement::Ptr& hit);

    /** @brief Dispatches a scroll event through the intercept + bubble pipeline. */
    InputResult dispatch_scroll(ScrollEvent& event, const IElement::Ptr& hit);

    /** @brief Updates hover state and fires enter/leave. */
    void update_hover(const IElement::Ptr& new_hover, const PointerEvent& event);

    weak_ptr<IScene> scene_;
    IElement::Ptr hovered_;
    IElement::Ptr pressed_;
    IElement::Ptr focused_;
    bool captured_ = false;
};

} // namespace velk::ui::impl

#endif // VELK_UI_INPUT_DISPATCHER_H
