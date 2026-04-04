#ifndef VELK_UI_API_VISUAL_RECT_H
#define VELK_UI_API_VISUAL_RECT_H

#include <velk-ui/api/visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a RectVisual (solid color rectangle).
 *
 * Inherits color and paint accessors from Visual.
 *
 *   auto rect = visual::create_rect();
 *   rect.set_color({0.9f, 0.2f, 0.2f, 1.f});
 */
class RectVisual : public Visual
{
public:
    /** @brief Default-constructed RectVisual wraps no object. */
    RectVisual() = default;

    /** @brief Wraps an existing IObject pointer. */
    explicit RectVisual(IObject::Ptr obj) : Visual(std::move(obj)) {}

    /** @brief Wraps an existing IVisual pointer. */
    explicit RectVisual(IVisual::Ptr v) : Visual(as_object(v)) {}
};

namespace visual {

/** @brief Creates a new RectVisual. */
inline RectVisual create_rect()
{
    return RectVisual(instance().create<IObject>(ClassId::Visual::Rect));
}

} // namespace visual

} // namespace velk::ui

#endif // VELK_UI_API_VISUAL_RECT_H
