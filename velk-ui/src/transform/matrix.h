#ifndef VELK_UI_MATRIX_TRANSFORM_H
#define VELK_UI_MATRIX_TRANSFORM_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_matrix.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Matrix : public ext::Transform<Matrix, IMatrix>
{
public:
    VELK_CLASS_UID(ClassId::Transform::Matrix, "Matrix");

    void transform(IElement& element) override;
};

} // namespace velk::ui

#endif // VELK_UI_MATRIX_TRANSFORM_H
