#ifndef VELK_UI_API_ELEMENT_H
#define VELK_UI_API_ELEMENT_H

#include <velk/api/hierarchy.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_render_trait.h>
#include <velk-ui/api/trait.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IElement.
 *
 * Inherits Node so elements carry hierarchy context (parent, children,
 * scene). All Node methods (get_parent, get_children, for_each_child, etc.)
 * work directly.
 *
 *   auto elem = create_element();
 *   elem.set_size({200.f, 100.f});
 *   auto parent = elem.get_parent();  // inherited from Node
 */
class Element : public Node
{
public:
    Element() = default;

    explicit Element(IObject::Ptr obj)
        : Node(make_node(std::move(obj)))
    {}

    explicit Element(IElement::Ptr e)
        : Node(make_node(e ? as_object(e) : IObject::Ptr{}))
    {}

    explicit Element(Node node)
        : Node(interface_cast<IElement>(node.object()) ? std::move(node) : Node{})
    {}

    operator IElement::Ptr() const { return as_ptr<IElement>(); }

    auto get_position() const { return read_state_value<IElement>(&IElement::State::position); }
    void set_position(const vec3& v) { write_state_value<IElement>(&IElement::State::position, v); }

    auto get_size() const { return read_state_value<IElement>(&IElement::State::size); }
    void set_size(const size& v) { write_state_value<IElement>(&IElement::State::size, v); }

    auto get_world_matrix() const { return read_state_value<IElement>(&IElement::State::world_matrix); }

    auto get_z_index() const { return read_state_value<IElement>(&IElement::State::z_index); }
    void set_z_index(int32_t v) { write_state_value<IElement>(&IElement::State::z_index, v); }

    /** @brief Attaches a trait (constraint, visual, etc.) to this element. */
    ReturnValue add_trait(const Trait& trait)
    {
        return trait ? add_attachment(trait.get()) : ReturnValue::InvalidArgument;
    }

    /** @brief Attaches a render trait (camera, light, ...) to this element. */
    ReturnValue add_trait(const ::velk::IRenderTrait::Ptr& trait)
    {
        return trait ? add_attachment(trait) : ReturnValue::InvalidArgument;
    }

    /** @brief Removes a previously attached trait from this element. */
    ReturnValue remove_trait(const Trait& trait)
    {
        return trait ? remove_attachment(trait.get()) : ReturnValue::InvalidArgument;
    }

    /** @brief Removes a previously attached render trait from this element. */
    ReturnValue remove_trait(const ::velk::IRenderTrait::Ptr& trait)
    {
        return trait ? remove_attachment(trait) : ReturnValue::InvalidArgument;
    }

    /** @brief Finds the first attached trait implementing interface T, or nullptr. */
    template <class T>
    typename T::Ptr find_trait() const { return find_attachment<T>(); }

    auto get_traits() const
    {
        vector<Trait> t;
        for (auto&& a : find_attachments<ITrait>()) {
            t.push_back(Trait(a));
        }
        return t;
    }

private:
    static HierarchyNode make_node(IObject::Ptr obj)
    {
        HierarchyNode n;
        if (auto* element = interface_cast<IElement>(obj)) {
            n.object = std::move(obj);
            n.hierarchy = interface_pointer_cast<IHierarchy>(element->get_scene());
        }
        return n;
    }
};

/** @brief Creates a new empty element with no layout constraints or visual representation. */
inline Element create_element()
{
    return Element(instance().create<IElement>(ClassId::Element));
}

} // namespace velk::ui

#endif // VELK_UI_API_ELEMENT_H
