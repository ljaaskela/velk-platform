#ifndef VELK_UI_API_VISUAL_SPHERE_H
#define VELK_UI_API_VISUAL_SPHERE_H

#include <velk-scene/api/visual/visual.h>
#include <velk-scene/plugin.h>

namespace velk {

class SphereVisual : public Visual3D
{
public:
    SphereVisual() = default;
    explicit SphereVisual(IObject::Ptr obj) : Visual3D(std::move(obj)) {}
    explicit SphereVisual(IVisual::Ptr v) : Visual3D(as_object(v)) {}
};

namespace trait::visual {

inline SphereVisual create_sphere()
{
    return SphereVisual(instance().create<IObject>(ClassId::Visual::Sphere));
}

} // namespace trait::visual

} // namespace velk

#endif // VELK_UI_API_VISUAL_SPHERE_H
