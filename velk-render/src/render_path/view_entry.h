#ifndef VELK_RENDER_VIEW_ENTRY_H
#define VELK_RENDER_VIEW_ENTRY_H

#include <velk-render/ext/render_state.h>
#include <velk-render/interface/intf_view_entry.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Concrete `IViewEntry` implementation. Hive-pooled.
 */
class ViewEntry : public ::velk::ext::RenderState<ViewEntry, ::velk::IViewEntry>
{
public:
    VELK_CLASS_UID(::velk::ClassId::ViewEntry, "ViewEntry");

    ::velk::IWindowSurface::Ptr surface() const override { return surface_; }
    void set_surface(::velk::IWindowSurface::Ptr surface) override
    {
        surface_ = std::move(surface);
    }

    ::velk::rect viewport() const override { return viewport_; }
    void set_viewport(::velk::rect viewport) override { viewport_ = viewport; }

    bool batches_dirty() const override { return batches_dirty_; }
    void set_batches_dirty(bool dirty) override { batches_dirty_ = dirty; }

    int cached_width() const override { return cached_width_; }
    int cached_height() const override { return cached_height_; }
    void set_cached_size(int width, int height) override
    {
        cached_width_ = width;
        cached_height_ = height;
    }

    void notify_batches_changed() override
    {
        notify_render_state_changed(::velk::RenderStateChange::All);
    }

private:
    ::velk::IWindowSurface::Ptr surface_;
    ::velk::rect viewport_{};
    bool batches_dirty_ = true;
    int cached_width_ = 0;
    int cached_height_ = 0;
};

} // namespace velk::impl

#endif // VELK_RENDER_VIEW_ENTRY_H
