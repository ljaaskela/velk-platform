#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-ui/element.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk_ui {

void Element::on_attached(IScene& scene)
{
    scene_ = &scene;
    pending_dirty_ = DirtyFlags::All;
    scene_->notify_dirty(*this, pending_dirty_);

    subscribe_visuals();
}

void Element::on_detached(IScene&)
{
    visual_subs_.clear();
    scene_ = nullptr;
    pending_dirty_ = DirtyFlags::None;
}

void Element::on_property_changed(velk::IProperty& property)
{
    if (!scene_) {
        return;
    }

    auto* meta = velk::interface_cast<velk::IMetadata>(this);
    if (!meta) {
        return;
    }

    auto members = meta->get_static_metadata();
    auto sid = property.get_storage_id();
    if (sid >= members.size()) {
        return;
    }

    auto name = members[sid].name;

    DirtyFlags flag = DirtyFlags::None;
    if (name == "position" || name == "size" || name == "local_transform") {
        flag = DirtyFlags::Layout;
    } else if (name == "z_index") {
        flag = DirtyFlags::ZOrder;
    }

    if (flag == DirtyFlags::None) {
        return;
    }

    bool was_clean = (pending_dirty_ == DirtyFlags::None);
    pending_dirty_ |= flag;

    if (was_clean) {
        scene_->notify_dirty(*this, flag);
    }
}

DirtyFlags Element::consume_dirty()
{
    DirtyFlags result = pending_dirty_;
    pending_dirty_ = DirtyFlags::None;
    return result;
}

void Element::subscribe_visuals()
{
    auto* storage = velk::interface_cast<velk::IObjectStorage>(this);
    if (!storage) {
        return;
    }

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);
        auto* visual = velk::interface_cast<IVisual>(att);
        if (!visual) {
            continue;
        }

        velk::Event evt = visual->on_visual_changed();
        if (!evt) {
            continue;
        }

        visual_subs_.emplace_back(evt, [this](velk::FnArgs) -> velk::ReturnValue {
            if (!scene_) {
                return velk::ReturnValue::Fail;
            }

            bool was_clean = (pending_dirty_ == DirtyFlags::None);
            pending_dirty_ |= DirtyFlags::Visual;
            if (was_clean) {
                scene_->notify_dirty(*this, DirtyFlags::Visual);
            }
            return velk::ReturnValue::Success;
        });
    }
}

} // namespace velk_ui
