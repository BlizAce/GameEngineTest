#include "Hollowpoint/Platform/InputEvent.h"

namespace Hollowpoint::Platform
{

// ============================================================================
// Key naming and text fallback.
//
// Both functions resolve ranges arithmetically rather than with one case per key: a new function key or
// digit then needs no new branch, and — the reason this matters — a forgotten branch cannot return the
// wrong name for an adjacent key. Only keys outside every range need explicit handling.
// ============================================================================

namespace
{

//! Names for the non-alphanumeric keypad entries, indexed by offset from NumPadDecimal.
constexpr const char* const kKeypadSymbols[] = {"KP_Decimal", "KP_Divide", "KP_Multiply", "KP_Subtract",
                                                "KP_Add",     "KP_Enter",  "KP_Equal"};

[[nodiscard]] const char* KeypadName(KeyCode key) noexcept
{
    const UInt16 offset = static_cast<UInt16>(key) - static_cast<UInt16>(KeyCode::NumPad0);

    // Ten digits first, then the operator/entry keys. Bounds are checked against each array's real size so a
    // value outside the keypad range cannot read past either table.
    if (offset <= 9u)
    {
        static constexpr const char* const digits[10] = {"KP_0", "KP_1", "KP_2", "KP_3", "KP_4",
                                                         "KP_5", "KP_6", "KP_7", "KP_8", "KP_9"};
        return digits[offset];
    }

    const UInt16 symbolIndex = offset - 10u;
    constexpr UInt16 symbolCount = static_cast<UInt16>(sizeof(kKeypadSymbols) / sizeof(kKeypadSymbols[0]));
    if (symbolIndex < symbolCount)
    {
        return kKeypadSymbols[symbolIndex];
    }

    return "";
}

}  // namespace

const char* KeyCodeName(KeyCode key) noexcept
{
    switch (key)
    {
        case KeyCode::Unknown:
            return "";
        case KeyCode::Space:
            return "Space";
        case KeyCode::Apostrophe:
            return "Apostrophe";
        case KeyCode::Comma:
            return "Comma";
        case KeyCode::Minus:
            return "Minus";
        case KeyCode::Period:
            return "Period";
        case KeyCode::Slash:
            return "Slash";
        case KeyCode::Semicolon:
            return "Semicolon";
        case KeyCode::Equal:
            return "Equal";
        case KeyCode::LeftBracket:
            return "LeftBracket";
        case KeyCode::Backslash:
            return "Backslash";
        case KeyCode::RightBracket:
            return "RightBracket";
        case KeyCode::GraveAccent:
            return "GraveAccent";
        case KeyCode::World1:
            return "World1";
        case KeyCode::World2:
            return "World2";
        case KeyCode::Escape:
            return "Escape";
        case KeyCode::Enter:
            return "Enter";
        case KeyCode::Tab:
            return "Tab";
        case KeyCode::Backspace:
            return "Backspace";
        case KeyCode::Insert:
            return "Insert";
        case KeyCode::Delete:
            return "Delete";
        case KeyCode::Right:
            return "Right";
        case KeyCode::Left:
            return "Left";
        case KeyCode::Down:
            return "Down";
        case KeyCode::Up:
            return "Up";
        case KeyCode::PageUp:
            return "PageUp";
        case KeyCode::PageDown:
            return "PageDown";
        case KeyCode::Home:
            return "Home";
        case KeyCode::End:
            return "End";
        case KeyCode::CapsLock:
            return "CapsLock";
        case KeyCode::ScrollLock:
            return "ScrollLock";
        case KeyCode::NumLock:
            return "NumLock";
        case KeyCode::PrintScreen:
            return "PrintScreen";
        case KeyCode::Pause:
            return "Pause";
        case KeyCode::LeftShift:
            return "LeftShift";
        case KeyCode::LeftControl:
            return "LeftControl";
        case KeyCode::LeftAlt:
            return "LeftAlt";
        case KeyCode::LeftSuper:
            return "LeftSuper";
        case KeyCode::RightShift:
            return "RightShift";
        case KeyCode::RightControl:
            return "RightControl";
        case KeyCode::RightAlt:
            return "RightAlt";
        case KeyCode::RightSuper:
            return "RightSuper";
        case KeyCode::Menu:
            return "Menu";
        default:
            break;  // Handled by the range logic below.
    }

    const auto value = static_cast<UInt16>(key);

    if (value >= static_cast<UInt16>(KeyCode::F1) && value <= static_cast<UInt16>(KeyCode::F25))
    {
        // Static storage per name: returning a formatted temporary would dangle, and there is a fixed, small
        // number of these.
        static constexpr const char* const names[25] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",
                                                        "F10", "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18",
                                                        "F19", "F20", "F21", "F22", "F23", "F24", "F25"};
        return names[value - static_cast<UInt16>(KeyCode::F1)];
    }

    if (value >= static_cast<UInt16>(KeyCode::FirstLetter) && value <= static_cast<UInt16>(KeyCode::LastLetter))
    {
        static constexpr const char* const letters[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I",
                                                          "J", "K", "L", "M", "N", "O", "P", "Q", "R",
                                                          "S", "T", "U", "V", "W", "X", "Y", "Z"};
        return letters[value - static_cast<UInt16>(KeyCode::FirstLetter)];
    }

    if (value >= static_cast<UInt16>(KeyCode::Digit1) && value <= static_cast<UInt16>(KeyCode::Digit0))
    {
        // The enum's digit range ends at Digit0, so this comparison is valid only because Digit0 > Digit1 —
        // asserted in the test suite so a future reordering cannot silently make it always-true.
        static constexpr const char* const digits[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
        return digits[value - static_cast<UInt16>(KeyCode::Digit1)];
    }

    if (value >= static_cast<UInt16>(KeyCode::NumPad0) && value <= static_cast<UInt16>(KeyCode::NumPadEqual))
    {
        return KeypadName(key);
    }

    return "";
}

char32_t PrintableCharacter(KeyCode key, KeyModifier modifiers) noexcept
{
    const bool shift = HasModifier(modifiers, KeyModifier::Shift);
    const auto value = static_cast<UInt16>(key);

    if (value >= static_cast<UInt16>(KeyCode::FirstLetter) && value <= static_cast<UInt16>(KeyCode::LastLetter))
    {
        const char32_t lower = static_cast<char32_t>('a' + (value - static_cast<UInt16>(KeyCode::FirstLetter)));
        return shift ? lower - 32 : lower;
    }

    if (value >= static_cast<UInt16>(KeyCode::Digit1) && value <= static_cast<UInt16>(KeyCode::Digit0))
    {
        const UInt16 index = value - static_cast<UInt16>(KeyCode::Digit1);
        const UInt16 digit = (index == 9u) ? 0u : static_cast<UInt16>(index + 1u);

        // Shifted digits are the US-layout symbols. This is a fallback for hosts that deliver no Character
        // events; on any other layout the symbol is wrong, which is why text entry must consume Character
        // events instead of calling this.
        static constexpr char32_t shifted[10] = {U')', U'!', U'@', U'#', U'$', U'%', U'^', U'&', U'*', U'('};
        return shift ? shifted[digit] : static_cast<char32_t>('0' + digit);
    }

    switch (key)
    {
        case KeyCode::Space:
            return U' ';
        case KeyCode::Enter:
            return U'\n';
        case KeyCode::Tab:
            return U'\t';
        case KeyCode::Comma:
            return shift ? U'<' : U',';
        case KeyCode::Period:
            return shift ? U'>' : U'.';
        case KeyCode::Slash:
            return shift ? U'?' : U'/';
        case KeyCode::Semicolon:
            return shift ? U':' : U';';
        case KeyCode::Apostrophe:
            return shift ? U'"' : U'\'';
        case KeyCode::Minus:
            return shift ? U'_' : U'-';
        case KeyCode::Equal:
            return shift ? U'+' : U'=';
        case KeyCode::LeftBracket:
            return shift ? U'{' : U'[';
        case KeyCode::RightBracket:
            return shift ? U'}' : U']';
        case KeyCode::Backslash:
            return shift ? U'|' : U'\\';
        case KeyCode::GraveAccent:
            return shift ? U'~' : U'`';
        default:
            return 0;
    }
}

}  // namespace Hollowpoint::Platform
