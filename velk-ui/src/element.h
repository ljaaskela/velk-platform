#ifndef VELK_UI_ELEMENT_H
#define VELK_UI_ELEMENT_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/interface/intf_metadata_observer.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_scene_observer.h>
#include <velk-ui/plugin.h>
#include <velk-ui/types.h>

namespace velk_ui {

class Element : public velk::ext::Object<Element, IElement, velk::IMetadataObserver, ISceneObserver>
{
public:
    VELK_CLASS_UID(ClassId::Element, "Element");

    void on_state_changed(velk::string_view name, velk::IMetadata& owner, velk::Uid interfaceId) override;

    void on_attached(IScene& scene) override;
    void on_detached(IScene& scene) override;

    velk::shared_ptr<IScene> get_scene() const override { return scene_.lock(); }
    DirtyFlags consume_dirty() override;

private:
    void subscribe_visuals();

    IScene::WeakPtr scene_;
    DirtyFlags pending_dirty_ = DirtyFlags::None;
    velk::vector<velk::ScopedHandler> visual_subs_;
};

} // namespace velk_ui

#endif // VELK_UI_ELEMENT_H
