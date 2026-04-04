#ifndef VELK_UI_API_VISUAL_ROUNDED_RECT_H
#define VELK_UI_API_VISUAL_ROUNDED_RECT_H

#include <velk-ui/api/visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a RoundedRectVisual.
 *
 * Inherits color and paint accessors from Visual.
 *
 *   auto rect = visual::create_rounded_rect();
 *   rect.set_color({0.2f, 0.6f, 0.9f, 1.f});
 */
class RoundedRectVisual : public Visual
{
public:
    RoundedRectVisual() = default;
    explicit RoundedRectVisual(IObject::Ptr obj) : Visual(std::move(obj)) {}
    explicit RoundedRectVisual(IVisual::Ptr v) : Visual(as_object(v)) {}
};

namespace visual {

/** @brief Creates a new RoundedRectVisual. */
inline RoundedRectVisual create_rounded_rect()
{
    return RoundedRectVisual(instance().create<IObject>(ClassId::Visual::RoundedRect));
}

} // namespace visual

} // namespace velk::ui

#endif // VELK_UI_API_VISUAL_ROUNDED_RECT_H
