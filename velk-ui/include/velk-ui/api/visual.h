#ifndef VELK_UI_API_VISUAL_H
#define VELK_UI_API_VISUAL_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/intf_material.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk_ui {

/**
 * @brief Base API wrapper for all visuals.
 *
 * Provides null-safe access to IVisual properties shared by all visual types:
 * color and paint. Concrete wrappers (RectVisual, TextVisual) inherit from this.
 */
class Visual : public Trait
{
public:
    /** @brief Default-constructed Visual wraps no object. */
    Visual() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IVisual. */
    explicit Visual(velk::IObject::Ptr obj) : Trait(check_object<IVisual>(obj)) {}

    /** @brief Implicit conversion to IVisual::Ptr. */
    operator IVisual::Ptr() const { return as_ptr<IVisual>(); }

    /** @brief Returns the base color. */
    auto get_color() const { return read_state_value<IVisual>(&IVisual::State::color); }

    /** @brief Sets the base color. Used when no paint is set. */
    void set_color(const velk::color& v) { write_state_value<IVisual>(&IVisual::State::color, v); }

    /** @brief Returns the paint (IMaterial reference), or empty. */
    auto get_paint() const { return read_state_value<IVisual>(&IVisual::State::paint); }

    /** @brief Sets a material as the paint, overriding the default color. */
    void set_paint(const IMaterial::Ptr& material)
    {
        write_state<IVisual>(
            [&](IVisual::State& s) { s.paint.set(interface_pointer_cast<velk::IObject>(material)); });
    }
};

} // namespace velk_ui

#endif // VELK_UI_API_VISUAL_H
