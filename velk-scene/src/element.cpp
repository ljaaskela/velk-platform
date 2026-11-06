#include "element.h"

#include <velk/interface/intf_metadata.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/interface/intf_render_to_texture.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-scene/interface/intf_trait.h>
#include <velk-scene/interface/intf_transform_trait.h>

namespace velk::impl {

void Element::on_attached(IScene& scene)
{
    auto s = interface_cast<IObject>(&scene)->get_self<IScene>();
    scene_ = s;
    pending_dirty_ = DirtyFlags::All;
    s->notify_dirty(*this, pending_dirty_);
    subscribe_traits();
}

void Element::on_detached(IScene& scene)
{
    if (registered_dependent_) {
        scene.set_depends_on_others(*this, false);
        registered_dependent_ = false;
    }
    dependent_trait_count_ = 0;
    trait_subs_.clear();
    scene_ = nullptr;
    pending_dirty_ = DirtyFlags::None;
}

void Element::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    // Property setters name the member; state writes pass an empty name, and
    // their writers report through mark_dirty.
    if (name == "position" || name == "size") {
        mark_dirty(DirtyFlags::Layout);
    } else if (name == "z_index") {
        mark_dirty(DirtyFlags::DrawOrder);
    }
}

void Element::mark_dirty(DirtyFlags flags)
{
    auto scene = get_scene();
    if (!scene) {
        return;
    }
    pending_dirty_ |= flags;
    // Scene::notify_dirty's dirty_elements_ list can take duplicate pushes.
    // consume_dirty clears pending_dirty_ on the first visit, so later visits
    // are no-ops.
    scene->notify_dirty(*this, flags);
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
        sync_dependent_registration();
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

    if (auto* transform = interface_cast<ITransformTrait>(attachment);
        transform && transform->depends_on_other_elements()) {
        dependent_trait_count_++;
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
    dependent_trait_count_ = 0;

    auto* storage = interface_cast<IObjectStorage>(this);
    if (storage) {
        for (size_t i = 0; i < storage->attachment_count(); ++i) {
            subscribe_trait(storage->get_attachment(i));
        }
    }
    sync_dependent_registration();
}

void Element::sync_dependent_registration()
{
    auto scene = get_scene();
    if (!scene) {
        return;
    }
    const bool depends = dependent_trait_count_ != 0;
    if (depends != registered_dependent_) {
        scene->set_depends_on_others(*this, depends);
        registered_dependent_ = depends;
    }
}

} // namespace velk::impl
