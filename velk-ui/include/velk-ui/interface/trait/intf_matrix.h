#ifndef VELK_UI_INTF_MATRIX_H
#define VELK_UI_INTF_MATRIX_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief Raw 4x4 matrix transform.
 *
 * Multiplied into the world matrix after layout is finalized.
 */
class IMatrix : public Interface<IMatrix>
{
public:
    VELK_INTERFACE(
        (PROP, mat4, matrix, {})
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_MATRIX_H
