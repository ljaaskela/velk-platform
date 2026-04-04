#ifndef VELK_RENDER_INTF_SURFACE_H
#define VELK_RENDER_INTF_SURFACE_H

#include <velk/interface/intf_metadata.h>

namespace velk {

class ISurface : public Interface<ISurface>
{
public:
    VELK_INTERFACE(
        (PROP, int, width, 0),
        (PROP, int, height, 0)
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_SURFACE_H
