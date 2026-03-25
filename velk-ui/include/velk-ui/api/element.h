#ifndef VELK_UI_API_ELEMENT_H
#define VELK_UI_API_ELEMENT_H

#include <velk/api/hierarchy.h>
#include <velk/api/state.h>

#include <velk-ui/api/trait.h>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_scene.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

/**
 * @brief Convenience wrapper around IElement.
 *
 * Inherits velk::Node so elements carry hierarchy context (parent, children,
 * scene). All Node methods (get_parent, get_children, for_each_child, etc.)
 * work directly.
 *
 *   auto elem = create_element();
 *   elem.set_size({200.f, 100.f});
 *   auto parent = elem.get_parent();  // inherited from Node
 */
class Element : public velk::Node
{
public:
    Element() = default;

    explicit Element(velk::IObject::Ptr obj)
        : Node(make_node(std::move(obj)))
    {}

    explicit Element(IElement::Ptr e)
        : Node(make_node(e ? interface_pointer_cast<velk::IObject>(e) : velk::IObject::Ptr{}))
    {}

    explicit Element(velk::Node node)
        : Node(interface_cast<IElement>(node.object()) ? std::move(node) : velk::Node{})
    {}

    operator IElement::Ptr() const { return as_ptr<IElement>(); }

    auto get_position() const { return read_state_value<IElement>(&IElement::State::position); }
    void set_position(const velk::vec3& v) { write_state_value<IElement>(&IElement::State::position, v); }

    auto get_size() const { return read_state_value<IElement>(&IElement::State::size); }
    void set_size(const velk::size& v) { write_state_value<IElement>(&IElement::State::size, v); }

    auto get_local_transform() const { return read_state_value<IElement>(&IElement::State::local_transform); }
    auto get_world_matrix() const { return read_state_value<IElement>(&IElement::State::world_matrix); }

    auto get_z_index() const { return read_state_value<IElement>(&IElement::State::z_index); }
    void set_z_index(int32_t v) { write_state_value<IElement>(&IElement::State::z_index, v); }

    /** @brief Attaches a trait (constraint, visual, etc.) to this element. */
    velk::ReturnValue add_trait(const Trait& trait)
    {
        return trait ? add_attachment(trait.get()) : velk::ReturnValue::InvalidArgument;
    }

    /** @brief Removes a previously attached trait from this element. */
    velk::ReturnValue remove_trait(const Trait& trait)
    {
        return trait ? remove_attachment(trait.get()) : velk::ReturnValue::InvalidArgument;
    }

    /** @brief Finds the first attached trait implementing interface T, or nullptr. */
    template <class T>
    typename T::Ptr find_trait() const { return find_attachment<T>(); }

    auto get_traits() const
    {
        velk::vector<Trait> t;
        for (auto&& a : find_attachments<ITrait>()) {
            t.push_back(Trait(a));
        }
        return t;
    }

private:
    static velk::HierarchyNode make_node(velk::IObject::Ptr obj)
    {
        velk::HierarchyNode n;
        if (auto* element = interface_cast<IElement>(obj)) {
            n.object = std::move(obj);
            n.hierarchy = interface_pointer_cast<velk::IHierarchy>(element->get_scene());
        }
        return n;
    }
};

/** @brief Creates a new empty element with no layout constraints or visual representation. */
inline Element create_element()
{
    return Element(velk::instance().create<IElement>(ClassId::Element));
}

} // namespace velk_ui

#endif // VELK_UI_API_ELEMENT_H
