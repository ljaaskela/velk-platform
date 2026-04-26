#ifndef VELK_UI_ELEMENT_H
#define VELK_UI_ELEMENT_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_scene_observer.h>
#include <velk-scene/plugin.h>
#include <velk-scene/types.h>

namespace velk::impl {

class Element : public ::velk::ext::Object<Element, IElement, IMetadataObserver, ISceneObserver>
{
public:
    VELK_CLASS_UID(::velk::ClassId::Element, "Element");

    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

    void on_attached(IScene& scene) override;
    void on_detached(IScene& scene) override;

    shared_ptr<IScene> get_scene() const override { return scene_.lock(); }
    DirtyFlags consume_dirty() override;
    bool has_render_traits() const override { return render_trait_count != 0; }

    ReturnValue add_attachment(const IInterface::Ptr& attachment) override;
    ReturnValue remove_attachment(const IInterface::Ptr& attachment) override;

private:
    void subscribe_traits();
    void subscribe_trait(const IInterface::Ptr& attachment);

    IScene::WeakPtr scene_;
    vector<ScopedHandler> trait_subs_;
    uint32_t render_trait_count{};
    DirtyFlags pending_dirty_ = DirtyFlags::None;
};

} // namespace velk::impl

#endif // VELK_UI_ELEMENT_H
