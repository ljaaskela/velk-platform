#ifndef VELK_UI_RENDERER_IMPL_H
#define VELK_UI_RENDERER_IMPL_H

#include "intf_renderer_internal.h"

#include <velk/ext/object.h>
#include <velk/interface/intf_property.h>
#include <velk/vector.h>

#include <unordered_map>
#include <velk-ui/interface/intf_material.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_texture_provider.h>
#include <velk-ui/plugins/render/intf_render_backend.h>
#include <velk-ui/plugins/render/plugin.h>
#include <velk-ui/types.h>

namespace velk_ui {

class Renderer : public velk::ext::Object<Renderer, IRendererInternal>
{
public:
    VELK_CLASS_UID(ClassId::Renderer, "Renderer");

    // IRendererInternal
    void set_backend(const IRenderBackend::Ptr& backend,
                     IRenderContext* ctx) override;

    // IRenderer
    void attach(const ISurface::Ptr& surface, const IScene::Ptr& scene) override;
    void detach(const ISurface::Ptr& surface) override;
    void render() override;
    void shutdown() override;

private:
    struct UniformBinding
    {
        int location = -1;
        velk::Uid typeUid;
        velk::IProperty::Ptr property;
    };

    struct VisualCommands
    {
        velk::vector<DrawEntry> entries;
        uint64_t pipeline_override = 0; ///< Non-zero if a material overrides the pipeline.
        IMaterial* material = nullptr;
        velk::vector<UniformBinding> uniform_bindings;
    };

    struct ElementCache
    {
        velk::vector<VisualCommands> visuals;
        ITextureProvider* texture_provider = nullptr;
    };

    struct SurfaceEntry
    {
        ISurface::Ptr surface;
        IScene::Ptr scene;
        uint64_t surface_id = 0;
        bool batches_dirty = true;
        int cached_width = 0;
        int cached_height = 0;
    };

    void rebuild_commands(IElement* element);
    void rebuild_batches(const SceneState& state, const SurfaceEntry& entry);

    IRenderBackend::Ptr backend_;
    IRenderContext* render_ctx_ = nullptr;
    velk::vector<SurfaceEntry> surfaces_;
    std::unordered_map<IElement*, ElementCache> element_cache_;

    std::unordered_map<uint64_t, size_t> batch_index_;
    velk::vector<RenderBatch> batches_;

    // Cached pipeline uniform info (introspected once per pipeline)
    std::unordered_map<uint64_t, velk::vector<UniformInfo>> pipeline_uniforms_;

    uint64_t next_surface_id_ = 1;

    void populate_uniform_bindings(VisualCommands& vc, IMaterial* mat);
    void fill_batch_uniforms(RenderBatch& batch, const VisualCommands& vc,
                             float x, float y, float w, float h) const;
};

} // namespace velk_ui

#endif // VELK_UI_RENDERER_IMPL_H
