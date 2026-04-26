#ifndef VELK_SCENE_EXT_TRAIT_H
#define VELK_SCENE_EXT_TRAIT_H

#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-render/interface/intf_raster_shader.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-scene/interface/intf_trait.h>
#include <velk-scene/interface/intf_transform_trait.h>
#include <velk-scene/interface/intf_visual.h>

namespace velk::ext {

/**
 * @brief Generic CRTP base for traits that participate in the Element
 *        dirty/notify protocol.
 *
 * Bundles the universal plumbing: ITraitNotify (so the element can
 * observe phase-dirty events) and IMetadataObserver (so property
 * writes flow through on_state_changed). Subclasses provide
 * `get_phase()` and the interface-specific defaults; they call
 * `invoke_trait_dirty(flags)` from their own `on_state_changed` to
 * push the right dirty flag.
 */
template <class T, class... Extra>
class Trait : public ::velk::ext::Object<T, ITraitNotify, IMetadataObserver, Extra...>
{
protected:
    void invoke_trait_dirty(DirtyFlags flags)
    {
        invoke_event(this->get_interface(IInterface::UID), "on_trait_dirty", flags);
    }
};

template <class T, class... Extra>
class Transform : public Trait<T, ITransformTrait, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Transform; }
    void transform(IElement&) override {}

protected:
    void on_state_changed(string_view, IMetadata&, Uid) override
    {
        this->invoke_trait_dirty(DirtyFlags::Layout);
    }
};

template <class T, class Variant, class... Extra>
class VisualBase : public Trait<T, Variant, ::velk::IRasterShader, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Visual; }

    ::velk::ShaderSource get_raster_source(::velk::IRasterShader::Target) const override
    {
        return {};
    }

    uint64_t get_raster_pipeline_key() const override { return PipelineKey::Default; }

    aabb get_local_bounds(const ::velk::size& bounds) const override
    {
        aabb out;
        out.position = {0.f, 0.f, 0.f};
        out.extent = {bounds.width, bounds.height, bounds.depth};
        return out;
    }

    vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext&) const override { return {}; }

protected:
    void invoke_visual_changed() { this->invoke_trait_dirty(DirtyFlags::Visual); }

    void on_state_changed(string_view, IMetadata&, Uid) override
    {
        this->invoke_trait_dirty(DirtyFlags::Visual);
    }
};

template <class T, class... Extra>
class Visual2D : public VisualBase<T, IVisual2D, Extra...> {};

template <class T, class... Extra>
class Visual3D : public VisualBase<T, IVisual3D, Extra...> {};

template <class T, class... Extra>
class Render : public Trait<T, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Render; }
};

} // namespace velk::ext

#endif // VELK_SCENE_EXT_TRAIT_H
