#ifndef VELK_UI_MATRIX_TRANSFORM_H
#define VELK_UI_MATRIX_TRANSFORM_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_matrix.h>
#include <velk-scene/plugin.h>

namespace velk {

class Matrix : public ext::Transform<Matrix, IMatrix>
{
public:
    VELK_CLASS_UID(ClassId::Transform::Matrix, "Matrix");

    mat4 transform(IElement& element, const mat4& world) override;
};

} // namespace velk

#endif // VELK_UI_MATRIX_TRANSFORM_H
