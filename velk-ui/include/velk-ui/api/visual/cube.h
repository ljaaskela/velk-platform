#ifndef VELK_UI_API_VISUAL_CUBE_H
#define VELK_UI_API_VISUAL_CUBE_H

#include <velk-ui/api/visual/visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class CubeVisual : public Visual
{
public:
    CubeVisual() = default;
    explicit CubeVisual(IObject::Ptr obj) : Visual(std::move(obj)) {}
    explicit CubeVisual(IVisual::Ptr v) : Visual(as_object(v)) {}
};

namespace trait::visual {

inline CubeVisual create_cube()
{
    return CubeVisual(instance().create<IObject>(ClassId::Visual::Cube));
}

} // namespace trait::visual

} // namespace velk::ui

#endif // VELK_UI_API_VISUAL_CUBE_H
