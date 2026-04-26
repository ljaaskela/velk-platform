#include "element.h"

#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/interface/intf_render_to_texture.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-scene/interface/intf_trait.h>

namespace velk::impl {

void Element::on_attached(IScene& scene)
{
    auto s = interface_cast<IObject>(&scene)->get_self<IScene>();
    scene_ = s;
    pending_dirty_ = DirtyFlags::All;
    s->notify_dirty(*this, pending_dirty_);
    subscribe_traits();
}

void Element::on_detached(IScene&)
{
    trait_subs_.clear();
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

    pending_dirty_ |= flag;
    // Always notify so the scene's aggregate `dirty_` sees every flag
    // (consumers like the pre-update layout pass query that aggregate
    // before any per-element consume runs). Scene::notify_dirty's
    // dirty_elements_ list can take duplicate pushes — consume_dirty
    // clears pending_dirty_ on the first visit so later visits become
    // no-ops without affecting redraw_list bookkeeping.
    scene->notify_dirty(*this, flag);
}

DirtyFlags Element::consume_dirty()
{
    DirtyFlags result = pending_dirty_;
    pending_dirty_ = DirtyFlags::None;
    return result;
}

ReturnValue Element::add_attachment(const IInterface::Ptr& attachment)
{
    auto rv = Object::add_attachment(attachment);
    if (succeeded(rv) && scene_.lock()) {
        subscribe_trait(attachment);
    }
    return rv;
}

ReturnValue Element::remove_attachment(const IInterface::Ptr& attachment)
{
    auto rv = Object::remove_attachment(attachment);
    if (succeeded(rv) && scene_.lock()) {
        // Re-subscribe from scratch: the removed trait's event handler is
        // now stale. This is simpler than tracking individual subscriptions
        // and only runs on structural changes (rare).
        if (interface_cast<IRenderToTexture>(attachment)) {
            render_trait_count--;
        }
        subscribe_traits();
    }
    return rv;
}

void Element::subscribe_trait(const IInterface::Ptr& attachment)
{
    if (interface_cast<IRenderToTexture>(attachment)) {
        render_trait_count++;
    }

    // Subscribe to ITraitNotify for any trait that fires dirty notifications.
    if (auto* notifier = interface_cast<ITraitNotify>(attachment)) {
        Event evt = notifier->on_trait_dirty();
        if (evt) {
            trait_subs_.emplace_back(evt, [this](const DirtyFlags& flags) -> ReturnValue {
                auto scene = get_scene();
                if (!scene) {
                    return ReturnValue::NothingToDo;
                }
                pending_dirty_ |= flags;
                scene->notify_dirty(*this, flags);
                return ReturnValue::Success;
            });
        }
    }
}

void Element::subscribe_traits()
{
    trait_subs_.clear();

    auto* storage = interface_cast<IObjectStorage>(this);
    if (storage) {
        for (size_t i = 0; i < storage->attachment_count(); ++i) {
            subscribe_trait(storage->get_attachment(i));
        }
    }
}

} // namespace velk::impl
