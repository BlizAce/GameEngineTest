#pragma once

#include <string>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

namespace Hollowpoint::Platform
{

using Core::Size;
using Core::UInt16;
using Core::UInt32;
using Core::UInt8;

// ---------------------------------------------------------------------------
// Input events.
//
// The engine's input model is an ordered queue of discrete events drained once per frame, rather than
// a "current state" snapshot polled by whoever wants it. Two reasons:
//
//   * **Determinism and replay.** A queued event stream can be recorded and fed back in to reproduce a
//     bug; scattered state polling cannot, because the order in which systems read keys changes between
//     runs.
//   * **Nothing is missed.** A windowing library delivers key presses as callbacks between frames; if
//     two presses of the same key happen inside one frame, a boolean state flag reports "down" once and
//     the second press is lost — the classic "inputs skip when the frame rate drops".
//
// Key codes are the engine's own enumeration rather than GLFW's: gameplay scripts and serialized
// bindings must not acquire a dependency on a windowing library's numeric values, which change between
// versions. The mapping from GLFW lives in one table in Platform/Src.
// ---------------------------------------------------------------------------

//! Physical key identifiers. Letters, digits, function keys and keypad digits each occupy a contiguous
//! range so callers can iterate them ("F1..F12", "0..9") without a per-name lookup table.
enum class KeyCode : UInt16
{
    Unknown = 0,

    Space,
    Apostrophe,
    Comma,
    Minus,
    Period,
    Slash,
    Semicolon,
    Equal,
    LeftBracket,
    Backslash,
    RightBracket,
    GraveAccent,
    World1,  //!< OS-dependent: usually the international backslash.
    World2,  //!< OS-dependent: usually the compose/menu key.

    Escape = 30,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right,
    Left,
    Down,
    Up,
    PageUp,
    PageDown,
    Home,
    End,
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,

    F1 = 50,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    F25,

    //! Keypad digits are deliberately separate from the top-row digits: a binding for "KP_5" and one for
    //! "5" must not collide.
    NumPad0 = 80,
    NumPad1,
    NumPad2,
    NumPad3,
    NumPad4,
    NumPad5,
    NumPad6,
    NumPad7,
    NumPad8,
    NumPad9,
    NumPadDecimal,
    NumPadDivide,
    NumPadMultiply,
    NumPadSubtract,
    NumPadAdd,
    NumPadEnter,
    NumPadEqual,

    LeftShift = 100,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    Menu,

    //! `A` .. `Z` occupy a contiguous range: `FirstLetter + n`.
    FirstLetter = 120,
    A = FirstLetter,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    LastLetter = Z,

    //! Top-row digits in reading order 1..9 then 0, which is what makes DigitKey(n) a constant offset.
    Digit1 = 150,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Digit0,

    Count
};

//! Mouse buttons. Separate from KeyCode because a binding system that conflates them cannot express
//! "any mouse button" or hold keyboard-only modifiers without inventing a fake key.
enum class MouseButton : UInt8
{
    Left = 0,
    Right = 1,
    Middle = 2,
    Forward = 3,
    Back = 4,
    Extra1 = 5,
    Extra2 = 6,
    Extra3 = 7,
    Extra4 = 8,

    Count
};

//! Modifier keys held during an event. A bitmask rather than separate events: modifiers are almost always
//! read as context ("Ctrl+S"), and delivering them as events would make every consumer track their own
//! modifier state anyway.
enum class KeyModifier : UInt8
{
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3,
};

[[nodiscard]] constexpr KeyModifier operator|(KeyModifier a, KeyModifier b) noexcept
{
    return static_cast<KeyModifier>(static_cast<UInt8>(a) | static_cast<UInt8>(b));
}

[[nodiscard]] constexpr KeyModifier operator&(KeyModifier a, KeyModifier b) noexcept
{
    return static_cast<KeyModifier>(static_cast<UInt8>(a) & static_cast<UInt8>(b));
}

[[nodiscard]] constexpr bool HasModifier(KeyModifier set, KeyModifier query) noexcept
{
    return (static_cast<UInt8>(set) & static_cast<UInt8>(query)) != 0;
}

//! What happened, for events with a press/release notion.
enum class InputAction : UInt8
{
    Release,
    Press,
    Repeat,  //!< OS auto-repeat. Distinct from Press so "repeat fires the weapon again" bugs are impossible.
};

//! Pointer position in **content coordinates**: pixels from the top-left of the window's drawable area,
//! already divided by the DPI scale factor. A window with a 2× scale factor and an 800x600 framebuffer
//! reports positions in 0..400 x 0..300 — that is what UI layout wants, and why raw framebuffer
//! coordinates are never exposed as input.
struct MouseMoveEvent
{
    float X = 0.0f;
    float Y = 0.0f;
};

//! One entry in the input queue. Kept small and trivially copyable (this is copied per event per frame),
//! with named members rather than a union so no consumer can read the payload of the wrong event type —
//! a union here would be undefined behaviour waiting for a missed tag check, and it is not big enough to
//! matter.
struct InputEvent
{
    enum class Type : UInt8
    {
        Key,
        Character,
        MouseButton,
        MouseMove,
        Scroll,
        WindowResize,
        Focus,
        Iconify,
        CloseRequested,
        DropFile,
    };

    Type EventType = Type::Key;

    KeyCode Key = KeyCode::Unknown;  //!< Key events.
    InputAction Action = InputAction::Press;
    KeyModifier Modifiers = KeyModifier::None;

    MouseButton Button = MouseButton::Left;  //!< MouseButton events.
    MouseMoveEvent Position;                 //!< MouseButton / MouseMove events.
    float ScrollX = 0.0f;                    //!< Scroll events, in lines, sign-correct.
    float ScrollY = 0.0f;

    UInt32 Width = 0;  //!< WindowResize events: new size in **framebuffer** pixels.
    UInt32 Height = 0;

    bool Focused = false;    //!< Focus events.
    bool Iconified = false;  //!< Iconify events.

    //! Character events: a decoded code point, so text entry works for any keyboard layout instead of
    //! assuming US key positions.
    char32_t Character = U'\0';

    std::string FilePath;  //!< DropFile events, UTF-8. One event per file.
};

//! Human-readable key name ("A", "F5", "LeftShift", "KP_7") for the binding editor and logs. Returns an
//! empty string for Unknown. Not localised: these name physical keys, not actions.
[[nodiscard]] HP_PLATFORM_API const char* KeyCodeName(KeyCode key) noexcept;

//! The top-row key for a digit 0..9 (so 0 maps to Digit0, matching the enum's reading order).
[[nodiscard]] constexpr KeyCode DigitKey(UInt8 digit) noexcept
{
    return digit == 0u ? KeyCode::Digit0 : static_cast<KeyCode>(static_cast<UInt16>(KeyCode::Digit1) + digit - 1u);
}

//! The letter for an index 0..25 (A..Z), or Unknown when out of range. Range-checked because callers
//! build this from data (a config file's key list) where an out-of-range value must not walk off the enum.
[[nodiscard]] constexpr KeyCode LetterKey(UInt8 index) noexcept
{
    return index < 26u ? static_cast<KeyCode>(static_cast<UInt16>(KeyCode::FirstLetter) + index) : KeyCode::Unknown;
}

//! Printable character for a key press under the current modifiers, or 0 when the key produces no text.
//! A fallback only: real text entry consumes Character events, which carry layout-correct code points and
//! therefore work on non-US keyboards where this function's symbol table would be wrong.
[[nodiscard]] HP_PLATFORM_API char32_t PrintableCharacter(KeyCode key, KeyModifier modifiers) noexcept;

}  // namespace Hollowpoint::Platform
