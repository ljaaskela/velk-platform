#ifndef VELK_UI_INPUT_TYPES_H
#define VELK_UI_INPUT_TYPES_H

#include <velk/api/math_types.h>

#include <cstdint>

namespace velk::ui {

/** @brief Result returned by input event handlers to control dispatch. */
enum class InputResult : uint8_t
{
    Ignored,  ///< Not handled. Continue bubbling.
    Consumed, ///< Handled. Stop bubbling.
    Captured  ///< Consumed and capture future pointer events until release.
};

/** @brief Keyboard modifier flags. */
enum class Modifier : uint8_t
{
    None  = 0,
    Shift = 1 << 0,
    Ctrl  = 1 << 1,
    Alt   = 1 << 2,
    Super = 1 << 3
};

inline constexpr Modifier operator|(Modifier a, Modifier b)
{
    return static_cast<Modifier>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr Modifier operator&(Modifier a, Modifier b)
{
    return static_cast<Modifier>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline constexpr bool operator!(Modifier a)
{
    return static_cast<uint8_t>(a) == 0;
}

/** @brief Pointer action type. */
enum class PointerAction : uint8_t
{
    Down,
    Up,
    Move,
    Cancel
};

/** @brief Pointer button identifier. */
enum class PointerButton : uint8_t
{
    None,
    Left,
    Right,
    Middle
};

/**
 * @brief Platform-agnostic pointer (mouse/touch) event.
 *
 * Position is in scene-space. The dispatcher fills local_position
 * with the element-local coordinate before delivery.
 */
struct PointerEvent
{
    vec2 position{};       ///< Scene-space pointer position.
    vec2 local_position{}; ///< Element-local position (filled by dispatcher).
    PointerButton button{};
    PointerAction action{};
    Modifier modifiers{};
    int pointer_id{};            ///< Pointer identifier (for multi-touch).
};

/** @brief Scroll distance unit. */
enum class ScrollUnit : uint8_t
{
    Pixels,
    Lines
};

/**
 * @brief Platform-agnostic scroll event.
 *
 * Position is in scene-space. The dispatcher fills local_position.
 */
struct ScrollEvent
{
    vec2 position{};       ///< Scene-space pointer position.
    vec2 local_position{}; ///< Element-local position (filled by dispatcher).
    vec2 delta{};          ///< Scroll amount (x = horizontal, y = vertical).
    ScrollUnit unit{};
    Modifier modifiers{};
};

/**
 * @brief Drag gesture event.
 *
 * Carries the gesture's spatial state. Delivered to drag event handlers
 * (on_drag_start, on_drag_move, on_drag_end). All positions are element-local.
 *
 * - on_drag_start: position = start_position, delta = {0,0}, total_delta = {0,0}
 * - on_drag_move:  position = current, delta = movement since last move,
 *                  total_delta = movement since start
 * - on_drag_end:   position = release point, delta = last move,
 *                  total_delta = full drag distance
 */
struct DragEvent
{
    vec2 start_position{}; ///< Where the drag began (element-local).
    vec2 position{};       ///< Current pointer position (element-local).
    vec2 delta{};          ///< Movement since the previous on_drag_move.
    vec2 total_delta{};    ///< Movement since on_drag_start.
    PointerButton button{};
    Modifier modifiers{};
};

/** @brief Key action type. */
enum class KeyAction : uint8_t
{
    Down,
    Up,
    Repeat,
    Text ///< A character was typed: codepoint is set, key is 0.
};

/** @brief Keys with a platform-independent meaning, for editing and navigation. */
enum class Key : uint8_t
{
    Unknown,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Enter,
    Escape,
    Tab
};

/**
 * @brief Platform-agnostic keyboard event.
 *
 * Dispatched to the focused element and bubbled up through ancestors.
 */
struct KeyEvent
{
    int key{};             ///< Platform key code.
    int scancode{};        ///< Hardware scan code.
    KeyAction action{};
    Modifier modifiers{};
    char32_t codepoint{};  ///< Unicode character, set for KeyAction::Text.
    Key named{};           ///< The key as a named key, Key::Unknown for the others.
};

} // namespace velk::ui

#endif // VELK_UI_INPUT_TYPES_H
