#ifndef VELK_UI_API_TRAIT_INSETS_H
#define VELK_UI_API_TRAIT_INSETS_H

#include <velk/api/state.h>

#include <velk-scene/api/trait.h>
#include <velk-ui/interface/trait/intf_insets.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IPadding.
 *
 *   auto padding = trait::layout::create_padding(dim::px(8.f));
 */
class Padding : public Trait
{
public:
    Padding() = default;
    explicit Padding(IObject::Ptr obj) : Trait(check_object<IPadding>(obj)) {}
    explicit Padding(IPadding::Ptr p) : Trait(as_object(p)) {}
    operator IPadding::Ptr() const { return as_ptr<IPadding>(); }

    /** @brief Sets all four sides. */
    void set(dim left, dim top, dim right, dim bottom)
    {
        write_state<IPadding>([&](auto& s) {
            s.left = left;
            s.top = top;
            s.right = right;
            s.bottom = bottom;
        });
    }
};

/**
 * @brief Convenience wrapper around IMargin.
 *
 *   auto margin = trait::layout::create_margin(dim::px(4.f));
 */
class Margin : public Trait
{
public:
    Margin() = default;
    explicit Margin(IObject::Ptr obj) : Trait(check_object<IMargin>(obj)) {}
    explicit Margin(IMargin::Ptr m) : Trait(as_object(m)) {}
    operator IMargin::Ptr() const { return as_ptr<IMargin>(); }

    /** @brief Sets all four sides. */
    void set(dim left, dim top, dim right, dim bottom)
    {
        write_state<IMargin>([&](auto& s) {
            s.left = left;
            s.top = top;
            s.right = right;
            s.bottom = bottom;
        });
    }
};

/**
 * @brief Convenience wrapper around IAlign.
 *
 *   auto align = trait::layout::create_align(Alignment::Center, Alignment::Center);
 */
class Align : public Trait
{
public:
    Align() = default;
    explicit Align(IObject::Ptr obj) : Trait(check_object<IAlign>(obj)) {}
    explicit Align(IAlign::Ptr a) : Trait(as_object(a)) {}
    operator IAlign::Ptr() const { return as_ptr<IAlign>(); }

    /** @brief Sets the horizontal and vertical alignment. Unset keeps the parent's. */
    void set(Alignment horizontal, Alignment vertical)
    {
        write_state<IAlign>([&](auto& s) {
            s.horizontal = horizontal;
            s.vertical = vertical;
        });
    }
};

namespace trait::layout {

/** @brief Creates a Padding with the same value on all four sides. */
inline Padding create_padding(dim all = dim::zero())
{
    auto p = Padding(instance().create<IPadding>(ClassId::Constraint::Padding));
    p.set(all, all, all, all);
    return p;
}

/** @brief Creates a Margin with the same value on all four sides. */
inline Margin create_margin(dim all = dim::zero())
{
    auto m = Margin(instance().create<IMargin>(ClassId::Constraint::Margin));
    m.set(all, all, all, all);
    return m;
}

/** @brief Creates an Align. */
inline Align create_align(Alignment horizontal = Alignment::Unset, Alignment vertical = Alignment::Unset)
{
    auto a = Align(instance().create<IAlign>(ClassId::Constraint::Align));
    a.set(horizontal, vertical);
    return a;
}

} // namespace trait::layout

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_INSETS_H
