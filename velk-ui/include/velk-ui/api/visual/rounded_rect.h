#ifndef VELK_UI_API_VISUAL_ROUNDED_RECT_H
#define VELK_UI_API_VISUAL_ROUNDED_RECT_H

#include <velk-scene/api/visual/visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a RoundedRectVisual.
 *
 * Inherits color and paint accessors from Visual.
 *
 *   auto rect = trait::visual::create_rounded_rect();
 *   rect.set_color({0.2f, 0.6f, 0.9f, 1.f});
 */
class RoundedRectVisual : public Visual2D
{
public:
    RoundedRectVisual() = default;
    explicit RoundedRectVisual(IObject::Ptr obj) : Visual2D(std::move(obj)) {}
    explicit RoundedRectVisual(IVisual::Ptr v) : Visual2D(as_object(v)) {}
};

namespace trait::visual {

/** @brief Creates a new RoundedRectVisual. */
inline RoundedRectVisual create_rounded_rect()
{
    return RoundedRectVisual(instance().create<IObject>(ClassId::Visual::RoundedRect));
}

} // namespace trait::visual

} // namespace velk::ui

#endif // VELK_UI_API_VISUAL_ROUNDED_RECT_H
