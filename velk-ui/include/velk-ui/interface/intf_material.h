#ifndef VELK_UI_INTF_MATERIAL_H
#define VELK_UI_INTF_MATERIAL_H

#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

namespace velk_ui {

/**
 * @brief Interface for custom materials that override the default color fill.
 *
 * A material defines how a visual's geometry is shaded. When a visual's `paint`
 * property references an IMaterial, the renderer uses the material instead of
 * the visual's `color` property.
 *
 * This is a plain interface, not a trait. Materials are always owned by a visual,
 * not attached directly to elements.
 */
class IMaterial : public velk::Interface<IMaterial>
{
public:
    VELK_INTERFACE(
        (PROP, velk::string, fragment_source, {})  ///< GLSL fragment shader source.
    )
};

} // namespace velk_ui

#endif // VELK_UI_INTF_MATERIAL_H
