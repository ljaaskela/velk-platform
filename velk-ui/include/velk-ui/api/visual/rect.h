#ifndef VELK_UI_API_VISUAL_RECT_H
#define VELK_UI_API_VISUAL_RECT_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/intf_visual.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Convenience wrapper around a RectVisual (IVisual with solid color fill).
 *
 * Provides null-safe access to visual properties.
 *
 *   auto rect = visual::create_rect();
 *   rect.set_color({0.9f, 0.2f, 0.2f, 1.f});
 */
class RectVisual : public Trait
{
public:
    /** @brief Default-constructed RectVisual wraps no object. */
    RectVisual() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IVisual. */
    explicit RectVisual(velk::IObject::Ptr obj)
        : Trait(obj && interface_cast<IVisual>(obj) ? std::move(obj) : velk::IObject::Ptr{})
    {}

    /** @brief Wraps an existing IVisual pointer. */
    explicit RectVisual(IVisual::Ptr v) : Trait(interface_pointer_cast<velk::IObject>(v)) {}

    /** @brief Implicit conversion to IVisual::Ptr. */
    operator IVisual::Ptr() const { return as_ptr<IVisual>(); }

    /** @brief Returns the fill color. */
    auto get_color() const { return read_state_value<IVisual>(&IVisual::State::color); }

    /** @brief Sets the fill color. */
    void set_color(const velk::color& v) { write_state_value<IVisual>(&IVisual::State::color, v); }
};

namespace visual {

/** @brief Creates a new RectVisual. */
inline RectVisual create_rect()
{
    return RectVisual(velk::instance().create<velk::IObject>(ClassId::Visual::Rect));
}

} // namespace visual

} // namespace velk_ui

#endif // VELK_UI_API_VISUAL_RECT_H
