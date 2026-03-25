#include "element.h"

#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk_ui {

void Element::on_attached(IScene& scene)
{
    auto s = interface_cast<velk::IObject>(&scene)->get_self<IScene>();
    scene_ = s;
    pending_dirty_ = DirtyFlags::All;
    s->notify_dirty(*this, pending_dirty_);
    subscribe_visuals();
}

void Element::on_detached(IScene&)
{
    visual_subs_.clear();
    scene_ = nullptr;
    pending_dirty_ = DirtyFlags::None;
}

void Element::on_state_changed(velk::string_view name, velk::IMetadata& owner, velk::Uid interfaceId)
{
    auto scene = get_scene();
    if (!scene) {
        return;
    }

    auto* meta = interface_cast<velk::IMetadata>(this);
    if (!meta) {
        return;
    }

    DirtyFlags flag = DirtyFlags::None;
    if (name == "position" || name == "size" || name == "local_transform") {
        flag = DirtyFlags::Layout;
    } else if (name == "z_index") {
        flag = DirtyFlags::DrawOrder;
    }

    if (flag == DirtyFlags::None) {
        return;
    }

    bool was_clean = (pending_dirty_ == DirtyFlags::None);
    pending_dirty_ |= flag;

    if (was_clean) {
        scene->notify_dirty(*this, flag);
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
    auto* storage = interface_cast<velk::IObjectStorage>(this);
    if (!storage) {
        return;
    }

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);
        auto* visual = interface_cast<IVisual>(att);
        if (!visual) {
            continue;
        }

        velk::Event evt = visual->on_visual_changed();
        if (!evt) {
            continue;
        }

        visual_subs_.emplace_back(evt, [this](velk::FnArgs) -> velk::ReturnValue {
            auto scene = get_scene();
            if (!scene) {
                return velk::ReturnValue::Fail;
            }

            bool was_clean = (pending_dirty_ == DirtyFlags::None);
            pending_dirty_ |= DirtyFlags::Visual;
            if (was_clean) {
                scene->notify_dirty(*this, DirtyFlags::Visual);
            }
            return velk::ReturnValue::Success;
        });
    }
}

} // namespace velk_ui
