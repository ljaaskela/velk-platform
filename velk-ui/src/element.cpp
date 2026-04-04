#include "element.h"

#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/interface/intf_visual.h>

namespace velk::ui {

void Element::on_attached(IScene& scene)
{
    auto s = interface_cast<IObject>(&scene)->get_self<IScene>();
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

void Element::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    auto scene = get_scene();
    if (!scene) {
        return;
    }

    auto* meta = interface_cast<IMetadata>(this);
    if (!meta) {
        return;
    }

    DirtyFlags flag = DirtyFlags::None;
    if (name == "position" || name == "size") {
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
    auto* storage = interface_cast<IObjectStorage>(this);
    if (!storage) {
        return;
    }

    for (size_t i = 0; i < storage->attachment_count(); ++i) {
        auto att = storage->get_attachment(i);
        auto* visual = interface_cast<IVisual>(att);
        if (!visual) {
            continue;
        }

        Event evt = visual->on_visual_changed();
        if (!evt) {
            continue;
        }

        visual_subs_.emplace_back(evt, [this](FnArgs) -> ReturnValue {
            auto scene = get_scene();
            if (!scene) {
                return ReturnValue::Fail;
            }

            bool was_clean = (pending_dirty_ == DirtyFlags::None);
            pending_dirty_ |= DirtyFlags::Visual;
            if (was_clean) {
                scene->notify_dirty(*this, DirtyFlags::Visual);
            }
            return ReturnValue::Success;
        });
    }
}

} // namespace velk::ui
