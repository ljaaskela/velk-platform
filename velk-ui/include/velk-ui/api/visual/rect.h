#ifndef VELK_UI_API_VISUAL_RECT_H
#define VELK_UI_API_VISUAL_RECT_H

#include <velk-scene/api/visual/visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a RectVisual (solid color rectangle).
 *
 * Inherits color and paint accessors from Visual.
 *
 *   auto rect = trait::visual::create_rect();
 *   rect.set_color({0.9f, 0.2f, 0.2f, 1.f});
 */
class RectVisual : public Visual2D
{
public:
    RectVisual() = default;
    explicit RectVisual(IObject::Ptr obj) : Visual2D(std::move(obj)) {}
    explicit RectVisual(IVisual::Ptr v) : Visual2D(as_object(v)) {}
};

namespace trait::visual {

/** @brief Creates a new RectVisual. */
inline RectVisual create_rect()
{
    return RectVisual(instance().create<IObject>(ClassId::Visual::Rect));
}

} // namespace trait::visual

} // namespace velk::ui

#endif // VELK_UI_API_VISUAL_RECT_H
