#ifndef VELK_UI_API_TRAIT_FIXED_SIZE_H
#define VELK_UI_API_TRAIT_FIXED_SIZE_H

#include <velk/api/state.h>

#include <velk-scene/api/trait.h>
#include <velk-ui/interface/trait/intf_fixed_size.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IFixedSize.
 *
 * Provides null-safe access to fixed size constraint properties.
 *
 *   auto fs = trait::layout::create_fixed_size();
 *   fs.set_width(dim::px(200.f));
 *   fs.set_height(dim::auto_size()); // fit the content
 *
 * dim::none() takes the space offered.
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

    /** @brief Returns the width. */
    auto get_width() const { return read_state_value<IFixedSize>(&IFixedSize::State::width); }

    /** @brief Sets the width. */
    void set_width(dim v) { write_state_value<IFixedSize>(&IFixedSize::State::width, v); }

    /** @brief Returns the height. */
    auto get_height() const { return read_state_value<IFixedSize>(&IFixedSize::State::height); }

    /** @brief Sets the height. */
    void set_height(dim v) { write_state_value<IFixedSize>(&IFixedSize::State::height, v); }

    /** @brief Returns the fixed depth. None = 0. */
    auto get_depth() const { return read_state_value<IFixedSize>(&IFixedSize::State::depth); }

    /** @brief Sets the fixed depth (used by 3D shape visuals such as cube/sphere). */
    void set_depth(dim v) { write_state_value<IFixedSize>(&IFixedSize::State::depth, v); }

    /** @brief Sets the width and height. */
    void set_size(dim w, dim h)
    {
        write_state<IFixedSize>([&](auto& s) {
            s.width = w;
            s.height = h;
        });
    }

    /** @brief Sets width, height, and depth in one call. */
    void set_size(dim w, dim h, dim d)
    {
        write_state<IFixedSize>([&](auto& s) {
            s.width = w;
            s.height = h;
            s.depth = d;
        });
    }
};

namespace trait::layout {

/** @brief Creates a new FixedSize constraint. */
inline FixedSize create_fixed_size(dim w = dim::none(), dim h = dim::none(),
                                   dim d = dim::none())
{
    auto fs = FixedSize(instance().create<IFixedSize>(ClassId::Constraint::FixedSize));
    if (w != dim::none() || h != dim::none() || d != dim::none()) {
        fs.set_size(w, h, d);
    }
    return fs;
}

} // namespace trait::layout

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_FIXED_SIZE_H
