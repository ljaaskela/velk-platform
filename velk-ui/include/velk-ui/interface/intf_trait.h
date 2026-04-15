#ifndef VELK_UI_INTF_TRAIT_H
#define VELK_UI_INTF_TRAIT_H

#include <velk/interface/intf_metadata.h>

#include <cstdint>
#include <velk-ui/types.h>

namespace velk::ui {

/**
 * @brief Bit flags for the element pipeline phases.
 *
 * A trait can participate in one or more phases. The solver tests
 * each trait's phase mask with bitwise AND.
 *
 * 1. Layout: walks children, divides space (e.g. Stack)
 * 2. Constraint: touches self only, refines size (e.g. FixedSize)
 * 3. Transform: modifies the world matrix after layout (e.g. Trs)
 * 4. Visual: produces draw commands for rendering (e.g. RectVisual)
 * 5. Render: defines how the scene is observed (e.g. Camera)
 */
enum class TraitPhase : uint8_t
{
    None       = 0,
    Layout     = 1 << 0, ///< Runs first. May read/write children via hierarchy.
    Constraint = 1 << 1, ///< Runs second. Touches only the element itself.
    Transform  = 1 << 2, ///< Runs third. Modifies the world matrix.
    Visual     = 1 << 3, ///< Runs last. Produces draw commands for rendering.
    Input      = 1 << 4, ///< Input handling. Does not participate in the layout solver.
    Render     = 1 << 5  ///< Render observation. Defines a view into the scene (e.g. Camera).
};

inline constexpr TraitPhase operator|(TraitPhase a, TraitPhase b)
{
    return static_cast<TraitPhase>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr TraitPhase operator&(TraitPhase a, TraitPhase b)
{
    return static_cast<TraitPhase>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline constexpr bool operator!(TraitPhase a)
{
    return static_cast<uint8_t>(a) == 0;
}

/**
 * @brief Base interface for UI traits attachable to elements.
 *
 * All element-attachable behaviors (layout, constraints, transforms, visuals)
 * inherit this interface. Enables uniform discovery and management via
 * Element::add_trait / remove_trait.
 */
class ITrait : public Interface<ITrait>
{
public:
    /** @brief Returns which phases of the element pipeline this trait participates in. */
    virtual TraitPhase get_phase() const = 0;
};

/** @brief Tests if a trait participates in the given phase. Null-safe. */
inline bool has_phase(const ITrait* trait, TraitPhase phase)
{
    return trait && static_cast<uint8_t>(trait->get_phase() & phase) != 0;
}

/**
 * @brief Common notification interface for traits that affect element state.
 *
 * Traits fire on_trait_dirty with the appropriate DirtyFlags when their
 * properties change. The owning element subscribes once and forwards
 * the flags to the scene.
 */
class ITraitNotify : public Interface<ITraitNotify>
{
public:
    VELK_INTERFACE(
        (EVT, on_trait_dirty, (DirtyFlags, flags))
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TRAIT_H
