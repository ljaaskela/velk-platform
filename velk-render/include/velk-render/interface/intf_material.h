#ifndef VELK_RENDER_INTF_MATERIAL_H
#define VELK_RENDER_INTF_MATERIAL_H

#include <velk-render/interface/intf_program.h>

namespace velk {

/**
 * @brief Marker interface for a renderable material.
 *
 * A material today is exactly a GPU program (pipeline + per-draw data);
 * all behaviour lives on `IProgram`. `IMaterial` exists as a named subtype
 * so visuals can reference "a material" via property and so future
 * material semantics (PBR surface description: albedo, roughness,
 * metalness) have a natural home without renaming the property type.
 *
 * Chain: IInterface -> IGpuResource -> IProgram -> IMaterial
 */
class IMaterial : public Interface<IMaterial, IProgram>
{
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_H
