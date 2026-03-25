#ifndef VELK_UI_INTF_TRAIT_H
#define VELK_UI_INTF_TRAIT_H

#include <velk/interface/intf_interface.h>

namespace velk_ui {

/**
 * @brief Marker interface for UI traits attachable to elements.
 *
 * Constraints (IStack, IFixedSize), visuals (IVisual), and any future
 * element-attachable behavior inherit this interface. Enables uniform
 * discovery and management via Element::add_trait / remove_trait.
 */
class ITrait : public velk::Interface<ITrait> {};

} // namespace velk_ui

#endif // VELK_UI_INTF_TRAIT_H
