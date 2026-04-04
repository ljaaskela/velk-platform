#ifndef VELK_RENDER_SURFACE_IMPL_H
#define VELK_RENDER_SURFACE_IMPL_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_surface.h>
#include <velk-render/plugin.h>

namespace velk {

class Surface : public ext::Object<Surface, ISurface>
{
public:
    VELK_CLASS_UID(ClassId::Surface, "Surface");
};

} // namespace velk

#endif // VELK_RENDER_SURFACE_IMPL_H
