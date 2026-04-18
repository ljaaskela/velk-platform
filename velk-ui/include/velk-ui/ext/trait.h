#ifndef VELK_UI_EXT_TRAIT_H
#define VELK_UI_EXT_TRAIT_H

#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-render/interface/intf_raster_shader.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-ui/interface/intf_trait.h>
#include <velk-ui/interface/intf_transform_trait.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk::ui::ext {

/**
 * @brief CRTP base for ILayoutTrait implementations.
 *
 * Provides default no-op measure/apply and a compile-time phase.
 * Includes ITraitNotify and IMetadataObserver: any property change
 * fires on_trait_dirty(Layout) so the owning element triggers a re-solve.
 */
template <class T, TraitPhase Phase, class... Extra>
class Layout : public ::velk::ext::Object<T, ILayoutTrait, ITraitNotify, IMetadataObserver, Extra...>
{
public:
    TraitPhase get_phase() const override { return Phase; }
    Constraint measure(const Constraint& c, IElement&, IHierarchy&) override { return c; }
    void apply(const Constraint&, IElement&, IHierarchy&) override {}

protected:
    void invoke_trait_dirty(DirtyFlags flags = DirtyFlags::Layout)
    {
        invoke_event(this->get_interface(IInterface::UID), "on_trait_dirty", flags);
    }

    void on_state_changed(string_view, IMetadata&, Uid) override { invoke_trait_dirty(); }
};

/**
 * @brief CRTP base for ITransformTrait implementations.
 *
 * Includes ITraitNotify and IMetadataObserver: any property change
 * fires on_trait_dirty(Layout) so the owning element triggers a re-solve
 * (transforms run during layout).
 */
template <class T, class... Extra>
class Transform : public ::velk::ext::Object<T, ITransformTrait, ITraitNotify, IMetadataObserver, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Transform; }
    void transform(IElement&) override {}

protected:
    void invoke_trait_dirty(DirtyFlags flags = DirtyFlags::Layout)
    {
        invoke_event(this->get_interface(IInterface::UID), "on_trait_dirty", flags);
    }

    void on_state_changed(string_view, IMetadata&, Uid) override { invoke_trait_dirty(); }
};

/**
 * @brief CRTP base for IVisual implementations.
 *
 * Bakes in IVisual, ITraitNotify, and IMetadataObserver. Provides
 * invoke_trait_dirty() and a default on_state_changed that fires Visual.
 *
 * @tparam T     The concrete visual class (CRTP parameter).
 * @tparam Extra Additional interfaces the visual implements.
 */
template <class T, class... Extra>
class Visual : public ::velk::ext::Object<T, IVisual, ::velk::IRasterShader,
                                          ITraitNotify, IMetadataObserver, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Visual; }

    /**
     * @brief Default: empty vertex+fragment for the requested target
     *        (renderer substitutes its registered defaults). Visuals
     *        that supply their own shader override this.
     */
    ::velk::ShaderSource get_raster_source(::velk::IRasterShader::Target) const override
    {
        return {};
    }

    /** @brief Default: `PipelineKey::Default`. Override for custom shader pipelines. */
    uint64_t get_raster_pipeline_key() const override { return PipelineKey::Default; }

protected:
    void invoke_trait_dirty(DirtyFlags flags = DirtyFlags::Visual)
    {
        invoke_event(this->get_interface(IInterface::UID), "on_trait_dirty", flags);
    }

    /** @brief Backward compat: fires on_trait_dirty(Visual). */
    void invoke_visual_changed() { invoke_trait_dirty(DirtyFlags::Visual); }

    void on_state_changed(string_view, IMetadata&, Uid) override { invoke_trait_dirty(); }
};

/**
 * @brief CRTP base for ITrait implementations running in
 *        `TraitPhase::Render` (e.g. RenderCache). Not used by
 *        IRenderTrait-based render traits (Camera, Light) — those
 *        inherit `ext::Object` directly since they're not ITraits.
 *
 * @tparam T     The concrete class (CRTP parameter).
 * @tparam Extra Additional interfaces (e.g. IRenderToTexture).
 */
template <class T, class... Extra>
class Render : public ::velk::ext::Object<T, ITraitNotify, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Render; }

protected:
    void invoke_trait_dirty(DirtyFlags flags = DirtyFlags::Visual)
    {
        invoke_event(this->get_interface(IInterface::UID), "on_trait_dirty", flags);
    }
};

} // namespace velk::ui::ext

#endif // VELK_UI_EXT_TRAIT_H
