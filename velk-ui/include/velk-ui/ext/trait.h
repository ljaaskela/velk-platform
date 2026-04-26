#ifndef VELK_UI_EXT_TRAIT_H
#define VELK_UI_EXT_TRAIT_H

#include <velk-scene/ext/trait.h>
#include <velk-ui/interface/intf_layout_trait.h>

namespace velk::ui::ext {

/**
 * @brief CRTP base for ILayoutTrait implementations.
 *
 * Provides default no-op measure/apply and a compile-time phase. Any
 * property change fires on_trait_dirty(Layout) so the owning element
 * triggers a re-solve.
 */
template <class T, TraitPhase Phase, class... Extra>
class LayoutTrait : public ::velk::ext::Trait<T, ILayoutTrait, Extra...>
{
public:
    TraitPhase get_phase() const override { return Phase; }
    Constraint measure(const Constraint& c, IElement&, IHierarchy&) override { return c; }
    void apply(const Constraint&, IElement&, IHierarchy&) override {}

protected:
    void on_state_changed(string_view, IMetadata&, Uid) override
    {
        this->invoke_trait_dirty(DirtyFlags::Layout);
    }
};

} // namespace velk::ui::ext

#endif // VELK_UI_EXT_TRAIT_H
