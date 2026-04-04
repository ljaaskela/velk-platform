#ifndef VELK_UI_INTF_GRADIENT_H
#define VELK_UI_INTF_GRADIENT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief Interface for linear gradient properties.
 *
 * Provides start color, end color, and angle for a linear gradient.
 * Properties are mapped to shader uniforms by the renderer via metadata
 * introspection (uniform names match property names with a u_ prefix).
 */
class IGradient : public Interface<IGradient>
{
public:
    VELK_INTERFACE(
        (PROP, color, start_color, (color::white())),
        (PROP, color, end_color, (color::black())),
        (PROP, float, angle, 0.f)
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_GRADIENT_H
