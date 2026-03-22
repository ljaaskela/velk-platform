#ifndef VELK_UI_EXT_VISUAL_H
#define VELK_UI_EXT_VISUAL_H

#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-ui/interface/intf_visual.h>

namespace velk_ui::ext {

/**
 * @brief CRTP base for IVisual implementations.
 *
 * Extends velk::ext::Object with IVisual and IMetadataObserver baked in.
 * Provides invoke_visual_changed() for subclasses and a default
 * on_property_changed that fires the event automatically.
 *
 * @tparam T The concrete visual class (CRTP parameter).
 * @tparam Extra Additional interfaces the visual implements (e.g. ITextureProvider).
 *
 * @par Example
 * @code
 * class RectVisual : public velk_ui::ext::Visual<RectVisual>
 * {
 * public:
 *     VELK_CLASS_UID(ClassId::Visual::Rect, "RectVisual");
 *     velk::vector<DrawCommand> get_draw_commands(const velk::rect& bounds) override;
 * };
 * @endcode
 */
template <class T, class... Extra>
class Visual : public velk::ext::Object<T, IVisual, velk::IMetadataObserver, Extra...>
{
protected:
    /** @brief Fires the on_visual_changed event. Call from subclasses when visual state changes. */
    void invoke_visual_changed()
    {
        auto evt = this->on_visual_changed();
        evt.invoke();
    }

    /** @brief Default: any property change fires on_visual_changed. Override to filter. */
    void on_property_changed(velk::IProperty&) override { invoke_visual_changed(); }
};

} // namespace velk_ui::ext

#endif // VELK_UI_EXT_VISUAL_H
