#include "stack.h"

#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_object_storage.h>

#include <algorithm>
#include <velk-ui/interface/intf_element.h>

namespace velk_ui {

ConstraintPhase Stack::get_phase() const
{
    return ConstraintPhase::Layout;
}

namespace {

struct ChildInfo
{
    velk::IObject::Ptr obj;
    IElement* element;
    velk::vector<IConstraint*> constraints;
    float measured_main = 0.f;
    bool fixed = false;
};

void collect_constraints(velk::IObject* obj, velk::vector<IConstraint*>& out)
{
    auto* storage = velk::interface_cast<velk::IObjectStorage>(obj);
    if (!storage) {
        return;
    }

    static constexpr velk::AttachmentQuery query{IConstraint::UID, {}};
    auto matches = storage->find_attachments(query);
    for (auto& att : matches) {
        auto* constraint = velk::interface_cast<IConstraint>(att);
        if (constraint) {
            out.push_back(constraint);
        }
    }
}

} // namespace

Constraint Stack::measure(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy)
{
    // Stack fills its parent bounds
    return c;
}

void Stack::apply(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy)
{
    if (!hierarchy) {
        return;
    }

    auto state = velk::read_state<IStack>(this);
    if (!state) {
        return;
    }

    uint8_t axis = state->axis; // 0 = horizontal, 1 = vertical
    float spacing = state->spacing;

    auto* obj = velk::interface_cast<velk::IObject>(&element);
    if (!obj) {
        return;
    }
    auto self = obj->get_self();
    auto children = hierarchy->children_of(self);
    if (children.empty()) {
        return;
    }

    float total_available = (axis == 1) ? c.bounds.extent.height : c.bounds.extent.width;
    float cross_available = (axis == 1) ? c.bounds.extent.width : c.bounds.extent.height;
    float total_spacing = spacing * static_cast<float>(children.size() - 1);
    float remaining = total_available - total_spacing;

    // Gather child info and run constraint-phase measure on each
    velk::vector<ChildInfo> infos;
    infos.reserve(children.size());

    for (auto& child_ptr : children) {
        ChildInfo info;
        info.obj = child_ptr;
        info.element = velk::interface_cast<IElement>(child_ptr);
        if (!info.element) {
            continue;
        }

        collect_constraints(child_ptr.get(), info.constraints);

        // Sort: Constraint-phase first for measuring
        std::sort(info.constraints.begin(), info.constraints.end(), [](IConstraint* a, IConstraint* b) {
            return static_cast<uint8_t>(a->get_phase()) > static_cast<uint8_t>(b->get_phase());
        });

        // Run measure on constraint-phase constraints to determine fixed sizes
        Constraint child_c;
        child_c.bounds.extent.width = (axis == 1) ? cross_available : remaining;
        child_c.bounds.extent.height = (axis == 1) ? remaining : cross_available;

        for (auto* con : info.constraints) {
            if (con->get_phase() == ConstraintPhase::Constraint) {
                child_c = con->measure(child_c, *info.element, hierarchy);
            }
        }

        float measured = (axis == 1) ? child_c.bounds.extent.height : child_c.bounds.extent.width;

        // Check if a constraint-phase constraint set a fixed size on the main axis
        float original = (axis == 1) ? remaining : remaining;
        if (measured != original) {
            info.fixed = true;
            info.measured_main = measured;
            remaining -= measured;
        }

        infos.push_back(std::move(info));
    }

    // Distribute remaining space to non-fixed children equally
    size_t flex_count = 0;
    for (auto& info : infos) {
        if (!info.fixed) {
            ++flex_count;
        }
    }

    float flex_size = (flex_count > 0 && remaining > 0.f) ? remaining / static_cast<float>(flex_count) : 0.f;

    for (auto& info : infos) {
        if (!info.fixed) {
            info.measured_main = flex_size;
        }
    }

    // Position children along the axis
    float cursor = (axis == 1) ? c.bounds.position.y : c.bounds.position.x;

    for (auto& info : infos) {
        float child_main = info.measured_main;
        float child_cross = cross_available;

        velk::write_state<IElement>(info.element, [&](IElement::State& s) {
            if (axis == 1) {
                s.position.x = c.bounds.position.x;
                s.position.y = cursor;
                s.size.width = child_cross;
                s.size.height = child_main;
            } else {
                s.position.x = cursor;
                s.position.y = c.bounds.position.y;
                s.size.width = child_main;
                s.size.height = child_cross;
            }
        });

        // Apply constraint-phase constraints on the child
        Constraint child_c;
        child_c.bounds.position = velk::read_state<IElement>(info.element)->position;
        child_c.bounds.extent.width = (axis == 1) ? child_cross : child_main;
        child_c.bounds.extent.height = (axis == 1) ? child_main : child_cross;

        for (auto* con : info.constraints) {
            if (con->get_phase() == ConstraintPhase::Constraint) {
                con->apply(child_c, *info.element, hierarchy);
            }
        }

        cursor += child_main + spacing;
    }
}

} // namespace velk_ui
