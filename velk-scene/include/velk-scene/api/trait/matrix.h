#ifndef VELK_UI_API_TRAIT_MATRIX_H
#define VELK_UI_API_TRAIT_MATRIX_H

#include <velk/api/state.h>

#include <velk-scene/api/trait.h>
#include <velk-scene/interface/trait/intf_matrix.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around IMatrix.
 *
 *   auto mtx = trait::transform::create_matrix();
 *   mtx.set_matrix(mat4::scale({2.f, 2.f, 1.f}));
 *   elem.add_trait(mtx);
 */
class Matrix : public Trait
{
public:
    Matrix() = default;
    explicit Matrix(IObject::Ptr obj) : Trait(check_object<IMatrix>(obj)) {}
    explicit Matrix(IMatrix::Ptr m) : Trait(as_object(m)) {}

    operator IMatrix::Ptr() const { return as_ptr<IMatrix>(); }

    auto get_matrix() const { return read_state_value<IMatrix>(&IMatrix::State::matrix); }
    void set_matrix(const mat4& v) { write_state_value<IMatrix>(&IMatrix::State::matrix, v); }
};

namespace trait::transform {

/** @brief Creates a new Matrix transform trait. */
inline Matrix create_matrix()
{
    return Matrix(instance().create<IMatrix>(ClassId::Transform::Matrix));
}

} // namespace trait::transform

} // namespace velk

#endif // VELK_UI_API_TRAIT_MATRIX_H
