#include "matrix.h"

#include <velk/api/state.h>

namespace velk {

mat4 Matrix::transform(IElement&, const mat4& world)
{
    const auto state = read_state<IMatrix>(this);
    return state ? world * state->matrix : world;
}

} // namespace velk
