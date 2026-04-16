#ifndef VELK_UI_EXT_TRAIT_H
#define VELK_UI_EXT_TRAIT_H

#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

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
class Visual : public ::velk::ext::Object<T, IVisual, ITraitNotify, IMetadataObserver, Extra...>
{
public:
    TraitPhase get_phase() const override { return TraitPhase::Visual; }

protected:
    void invoke_trait_dirty(DirtyFlags flags = DirtyFlags::Visual)
    {
        invoke_event(this->get_interface(IInterface::UID), "on_trait_dirty", flags);
    }

    /** @brief Backward compat: fires on_trait_dirty(Visual). */
    void invoke_visual_changed() { invoke_trait_dirty(DirtyFlags::Visual); }

    void on_state_changed(string_view, IMetadata&, Uid) override { invoke_trait_dirty(); }

    /** @brief Default pipeline key 1 = "filled rect". */
    uint64_t get_pipeline_key() const override { return PipelineKey::Default; }

    /** @brief Empty vertex shader = rect */
    string_view get_vertex_src() const override { return {}; }

    /** @brief Empty fragment shader = fill with IVisual::color */
    string_view get_fragment_src() const override { return {}; }

    /** @brief Empty intersect shader = intersect AABB. */
    string_view get_intersect_src() const override { return {}; }
};

/**
 * @brief CRTP base for Render-phase traits (e.g. Camera, RenderCache).
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
