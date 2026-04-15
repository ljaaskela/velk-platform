#ifndef VELK_RENDER_WINDOW_SURFACE_IMPL_H
#define VELK_RENDER_WINDOW_SURFACE_IMPL_H

#include <velk/api/state.h>
#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>

namespace velk {

class WindowSurface : public ext::GpuResource<WindowSurface, IWindowSurface>
{
public:
    VELK_CLASS_UID(ClassId::WindowSurface, "WindowSurface");

    GpuResourceType get_type() const override { return GpuResourceType::Surface; }
    uint64_t get_render_target_id() const override { return render_target_id_; }
    void set_render_target_id(uint64_t id) override { render_target_id_ = id; }

    uvec2 get_dimensions() const override
    {
        auto s = read_state<IWindowSurface>(this);
        return s ? s->size : uvec2{};
    }

    PixelFormat format() const override { return format_; }
    void set_format(PixelFormat fmt) { format_ = fmt; }

private:
    uint64_t render_target_id_ = 0;
    PixelFormat format_ = PixelFormat::RGBA8;
};

} // namespace velk

#endif // VELK_RENDER_WINDOW_SURFACE_IMPL_H
