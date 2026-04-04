#ifndef VELK_UI_API_TRAIT_FIXED_SIZE_H
#define VELK_UI_API_TRAIT_FIXED_SIZE_H

#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/trait/intf_fixed_size.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IFixedSize.
 *
 * Provides null-safe access to fixed size constraint properties.
 *
 *   auto fs = constraint::create_fixed_size();
 *   fs.set_width(dim::px(200.f));
 *   fs.set_height(dim::pct(50.f));
 */
class FixedSize : public Trait
{
public:
    /** @brief Default-constructed FixedSize wraps no object. */
    FixedSize() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IFixedSize. */
    explicit FixedSize(IObject::Ptr obj) : Trait(check_object<IFixedSize>(obj)) {}

    /** @brief Wraps an existing IFixedSize pointer. */
    explicit FixedSize(IFixedSize::Ptr f) : Trait(as_object(f)) {}

    /** @brief Implicit conversion to IFixedSize::Ptr. */
    operator IFixedSize::Ptr() const { return as_ptr<IFixedSize>(); }

    /** @brief Returns the fixed width. None = unconstrained. */
    auto get_width() const { return read_state_value<IFixedSize>(&IFixedSize::State::width); }

    /** @brief Sets the fixed width. Use dim::none() to leave unconstrained. */
    void set_width(dim v) { write_state_value<IFixedSize>(&IFixedSize::State::width, v); }

    /** @brief Returns the fixed height. None = unconstrained. */
    auto get_height() const { return read_state_value<IFixedSize>(&IFixedSize::State::height); }

    /** @brief Sets the fixed height. Use dim::none() to leave unconstrained. */
    void set_height(dim v) { write_state_value<IFixedSize>(&IFixedSize::State::height, v); }

    /** @brief Sets the fixed width and height. Use dim::none() to leave unconstrained. */
    void set_size(dim w, dim h)
    {
        ::write_state<IFixedSize>(as<IFixedSize>(), [&](auto& s) {
            s.width = w;
            s.height = h;
        });
    }
};

namespace constraint {

/** @brief Creates a new FixedSize constraint. */
inline FixedSize create_fixed_size()
{
    return FixedSize(instance().create<IFixedSize>(ClassId::Constraint::FixedSize));
}

} // namespace constraint

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_FIXED_SIZE_H
