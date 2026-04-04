#include "input_dispatcher.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object.h>
#include <velk/interface/intf_object_storage.h>

#ifdef VELK_INPUT_DEBUG
#define INPUT_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define INPUT_LOG(...) ((void)0)
#endif

namespace velk::ui::impl {

namespace {

/** @brief Returns an IElement::Ptr from a raw IElement*, or empty if null. */
IElement::Ptr to_ptr(IElement* elem)
{
    if (!elem) {
        return {};
    }
    auto* obj = interface_cast<IObject>(elem);
    return obj ? obj->get_self<IElement>() : IElement::Ptr{};
}

} // namespace

void InputDispatcher::set_scene(const shared_ptr<IScene>& scene)
{
    INPUT_LOG("set_scene: %s", scene ? "valid" : "null");
    scene_ = scene;
}

// ============================================================================
// IInputDispatcher
// ============================================================================

void InputDispatcher::pointer_event(const PointerEvent& event)
{
    auto scene = scene_.lock();
    if (!scene) {
        INPUT_LOG("pointer_event: scene expired");
        return;
    }

    PointerEvent ev = event;

    // If we have a captured element, route directly to it
    if (captured_ && pressed_) {
        auto* pressed_raw = pressed_.get();
        ev.local_position = to_local(pressed_raw, ev.position);

        auto* trait = get_input_trait(pressed_raw);
        if (trait) {
            trait->on_pointer_event(ev);
        }

        // Release capture on pointer up or cancel
        if (ev.action == PointerAction::Up || ev.action == PointerAction::Cancel) {
            captured_ = false;
            pressed_ = {};
        }

        // Update hover even while captured
        IElement* current_hover = hit_test(ev.position);
        update_hover(current_hover, ev);
        return;
    }

    IElement* hit = hit_test(ev.position);

    if (ev.action == PointerAction::Down) {
        INPUT_LOG("pointer down at (%.0f, %.0f), hit=%s", ev.position.x, ev.position.y,
                  hit ? "found" : "miss");
    }

    // Update hover state
    update_hover(hit, ev);

    if (!hit) {
        // Pointer down on empty space clears press
        if (ev.action == PointerAction::Down) {
            pressed_ = {};
        }
        return;
    }

    // Dispatch through intercept + bubble
    InputResult result = dispatch_pointer(ev, hit);
    if (ev.action == PointerAction::Down || ev.action == PointerAction::Up) {
        INPUT_LOG("dispatch result: %s (action=%s)",
                  result == InputResult::Ignored ? "Ignored" : result == InputResult::Consumed ? "Consumed" : "Captured",
                  ev.action == PointerAction::Down ? "Down" : "Up");
    }

    // Track press/capture state
    if (ev.action == PointerAction::Down) {
        pressed_ = to_ptr(hit);
        captured_ = (result == InputResult::Captured);
    } else if (ev.action == PointerAction::Up || ev.action == PointerAction::Cancel) {
        pressed_ = {};
        captured_ = false;
    }
}

void InputDispatcher::scroll_event(const ScrollEvent& event)
{
    auto scene = scene_.lock();
    if (!scene) {
        return;
    }

    ScrollEvent ev = event;
    IElement* hit = hit_test(ev.position);
    if (!hit) {
        return;
    }

    dispatch_scroll(ev, hit);
}

void InputDispatcher::key_event(const KeyEvent& event)
{
    // Key events dispatch to focused element and bubble up.
    // Stub for now: full implementation comes with focus management.
    if (!focused_) {
        return;
    }

    KeyEvent ev = event;
    auto* focused_raw = focused_.get();

    vector<IElement*> chain;
    build_ancestor_chain(focused_raw, chain);

    // Bubble: focused element first, then ancestors
    auto* trait = get_input_trait(focused_raw);
    if (trait) {
        InputResult r = trait->on_key_event(ev);
        if (r != InputResult::Ignored) {
            return;
        }
    }

    for (auto* ancestor : chain) {
        auto* at = get_input_trait(ancestor);
        if (at) {
            InputResult r = at->on_key_event(ev);
            if (r != InputResult::Ignored) {
                return;
            }
        }
    }
}

IElement::Ptr InputDispatcher::get_hovered() const
{
    return hovered_;
}

IElement::Ptr InputDispatcher::get_pressed() const
{
    return pressed_;
}

IElement::Ptr InputDispatcher::get_focused() const
{
    return focused_;
}

void InputDispatcher::set_focus(const IElement::Ptr& element)
{
    if (focused_ == element) {
        return;
    }
    focused_ = element;
    invoke_event(this->get_interface(IInterface::UID), "on_focus_changed");
}

// ============================================================================
// Hit testing
// ============================================================================

IElement* InputDispatcher::hit_test(vec2 point) const
{
    auto scene = scene_.lock();
    if (!scene) {
        return nullptr;
    }

    // Walk the visual list in reverse z-order (topmost first)
    auto list = scene->get_visual_list();
    for (size_t i = list.size(); i > 0; --i) {
        IElement* elem = list[i - 1];
        if (!get_input_trait(elem)) {
            continue;
        }

        rect wr = get_world_rect(elem);
        if (point.x >= wr.x && point.x < wr.x + wr.width &&
            point.y >= wr.y && point.y < wr.y + wr.height) {
            return elem;
        }
    }

    return nullptr;
}

IInputTrait* InputDispatcher::get_input_trait(IElement* element)
{
    if (!element) {
        return nullptr;
    }

    auto* storage = interface_cast<IObjectStorage>(element);
    if (!storage) {
        return nullptr;
    }

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);
        auto* trait = interface_cast<IInputTrait>(att);
        if (trait) {
            return trait;
        }
    }

    return nullptr;
}

rect InputDispatcher::get_world_rect(IElement* element)
{
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return {};
    }

    // Extract translation from world matrix (columns 12, 13)
    const auto& wm = reader->world_matrix;
    float wx = wm.m[12];
    float wy = wm.m[13];

    return {wx, wy, reader->size.width, reader->size.height};
}

vec2 InputDispatcher::to_local(IElement* element, vec2 scene_point)
{
    auto reader = read_state<IElement>(element);
    if (!reader) {
        return scene_point;
    }

    // Subtract world translation to get element-local coordinates
    const auto& wm = reader->world_matrix;
    return {scene_point.x - wm.m[12], scene_point.y - wm.m[13]};
}

// ============================================================================
// Dispatch
// ============================================================================

void InputDispatcher::build_ancestor_chain(IElement* target, vector<IElement*>& chain) const
{
    auto scene = scene_.lock();
    if (!scene) {
        return;
    }

    chain.clear();

    // Walk from target's parent to root, collecting ancestors with input traits
    auto* obj = interface_cast<IObject>(target);
    if (!obj) {
        return;
    }

    auto current = obj->get_self();
    while (true) {
        auto parent = scene->parent_of(current);
        if (!parent) {
            break;
        }

        auto* parent_elem = interface_cast<IElement>(parent);
        if (parent_elem && get_input_trait(parent_elem)) {
            chain.push_back(parent_elem);
        }

        current = parent;
    }
}

InputResult InputDispatcher::dispatch_pointer(PointerEvent& event, IElement* hit)
{
    vector<IElement*> ancestors;
    build_ancestor_chain(hit, ancestors);

    // Intercept pass: top-down (root to target)
    // ancestors are ordered from parent to root, so walk in reverse
    for (size_t i = ancestors.size(); i > 0; --i) {
        IElement* ancestor = ancestors[i - 1];
        event.local_position = to_local(ancestor, event.position);

        auto* trait = get_input_trait(ancestor);
        InputResult r = trait->on_intercept(event);
        if (r != InputResult::Ignored) {
            // Ancestor stole the event; it becomes the new target
            event.local_position = to_local(ancestor, event.position);
            trait->on_pointer_event(event);
            return r;
        }
    }

    // Bubble pass: target first, then ancestors (parent to root)
    event.local_position = to_local(hit, event.position);
    auto* hit_trait = get_input_trait(hit);
    if (hit_trait) {
        InputResult r = hit_trait->on_pointer_event(event);
        if (r != InputResult::Ignored) {
            return r;
        }
    }

    for (auto* ancestor : ancestors) {
        event.local_position = to_local(ancestor, event.position);
        auto* trait = get_input_trait(ancestor);
        InputResult r = trait->on_pointer_event(event);
        if (r != InputResult::Ignored) {
            return r;
        }
    }

    return InputResult::Ignored;
}

InputResult InputDispatcher::dispatch_scroll(ScrollEvent& event, IElement* hit)
{
    vector<IElement*> ancestors;
    build_ancestor_chain(hit, ancestors);

    // Bubble pass: target first, then ancestors
    event.local_position = to_local(hit, event.position);
    auto* hit_trait = get_input_trait(hit);
    if (hit_trait) {
        InputResult r = hit_trait->on_scroll_event(event);
        if (r != InputResult::Ignored) {
            return r;
        }
    }

    for (auto* ancestor : ancestors) {
        event.local_position = to_local(ancestor, event.position);
        auto* trait = get_input_trait(ancestor);
        InputResult r = trait->on_scroll_event(event);
        if (r != InputResult::Ignored) {
            return r;
        }
    }

    return InputResult::Ignored;
}

// ============================================================================
// Hover
// ============================================================================

void InputDispatcher::update_hover(IElement* new_hover, const PointerEvent& event)
{
    if (new_hover == hovered_.get()) {
        return;
    }

    auto* old_hover = hovered_.get();
    hovered_ = to_ptr(new_hover);

    if (old_hover) {
        auto* trait = get_input_trait(old_hover);
        if (trait) {
            PointerEvent leave_ev = event;
            leave_ev.local_position = to_local(old_hover, event.position);
            trait->on_pointer_leave(leave_ev);
        }
    }

    if (new_hover) {
        auto* trait = get_input_trait(new_hover);
        if (trait) {
            PointerEvent enter_ev = event;
            enter_ev.local_position = to_local(new_hover, event.position);
            trait->on_pointer_enter(enter_ev);
        }
    }
}

} // namespace velk::ui::impl
