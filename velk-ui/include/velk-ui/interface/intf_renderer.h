#ifndef VELK_UI_INTF_RENDERER_H
#define VELK_UI_INTF_RENDERER_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_surface.h>
#include <velk-ui/interface/intf_scene.h>

namespace velk::ui {

/**
 * @brief Scene renderer / dispatcher.
 *
 * Walks the scene tree, collects draw entries from visuals, batches them,
 * writes GPU buffers, and submits draw calls to the render backend.
 *
 * Created via velk_ui::create_renderer(ctx).
 */
class IRenderer : public Interface<IRenderer>
{
public:
    virtual void attach(const ISurface::Ptr& surface, const IScene::Ptr& scene) = 0;
    virtual void detach(const ISurface::Ptr& surface) = 0;
    virtual void render() = 0;
    virtual void shutdown() = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_RENDERER_H
