#include "GlfwInputMapping.h"

#include <vector>

namespace Hollowpoint::Platform::Detail
{

using Core::Size;

namespace
{

//! Highest GLFW key code we map. Anything above is treated as unmapped rather than indexing past the
//! table's end. GLFW_KEY_LAST tracks the highest defined key, so this follows GLFW automatically.
constexpr int kGlfwKeyRange = GLFW_KEY_LAST;

[[nodiscard]] const std::vector<KeyCode>& BuildGlfwToEngineTable()
{
    static const std::vector<KeyCode> table = []() {
        std::vector<KeyCode> map(static_cast<std::size_t>(kGlfwKeyRange) + 1u, KeyCode::Unknown);

        // clang-format off
        map[GLFW_KEY_SPACE]         = KeyCode::Space;
        map[GLFW_KEY_APOSTROPHE]    = KeyCode::Apostrophe;
        map[GLFW_KEY_COMMA]         = KeyCode::Comma;
        map[GLFW_KEY_MINUS]         = KeyCode::Minus;
        map[GLFW_KEY_PERIOD]        = KeyCode::Period;
        map[GLFW_KEY_SLASH]         = KeyCode::Slash;
        map[GLFW_KEY_SEMICOLON]     = KeyCode::Semicolon;
        map[GLFW_KEY_EQUAL]         = KeyCode::Equal;
        map[GLFW_KEY_LEFT_BRACKET]  = KeyCode::LeftBracket;
        map[GLFW_KEY_BACKSLASH]     = KeyCode::Backslash;
        map[GLFW_KEY_RIGHT_BRACKET] = KeyCode::RightBracket;
        map[GLFW_KEY_GRAVE_ACCENT]  = KeyCode::GraveAccent;
        map[GLFW_KEY_WORLD_1]       = KeyCode::World1;
        map[GLFW_KEY_WORLD_2]       = KeyCode::World2;

        map[GLFW_KEY_ESCAPE]        = KeyCode::Escape;
        map[GLFW_KEY_ENTER]         = KeyCode::Enter;
        map[GLFW_KEY_TAB]           = KeyCode::Tab;
        map[GLFW_KEY_BACKSPACE]     = KeyCode::Backspace;
        map[GLFW_KEY_INSERT]        = KeyCode::Insert;
        map[GLFW_KEY_DELETE]        = KeyCode::Delete;
        map[GLFW_KEY_RIGHT]         = KeyCode::Right;
        map[GLFW_KEY_LEFT]          = KeyCode::Left;
        map[GLFW_KEY_DOWN]          = KeyCode::Down;
        map[GLFW_KEY_UP]            = KeyCode::Up;
        map[GLFW_KEY_PAGE_UP]       = KeyCode::PageUp;
        map[GLFW_KEY_PAGE_DOWN]     = KeyCode::PageDown;
        map[GLFW_KEY_HOME]          = KeyCode::Home;
        map[GLFW_KEY_END]           = KeyCode::End;
        map[GLFW_KEY_CAPS_LOCK]     = KeyCode::CapsLock;
        map[GLFW_KEY_SCROLL_LOCK]   = KeyCode::ScrollLock;
        map[GLFW_KEY_NUM_LOCK]      = KeyCode::NumLock;
        map[GLFW_KEY_PRINT_SCREEN]  = KeyCode::PrintScreen;
        map[GLFW_KEY_PAUSE]         = KeyCode::Pause;

        map[GLFW_KEY_F1]  = KeyCode::F1;   map[GLFW_KEY_F2]  = KeyCode::F2;
        map[GLFW_KEY_F3]  = KeyCode::F3;   map[GLFW_KEY_F4]  = KeyCode::F4;
        map[GLFW_KEY_F5]  = KeyCode::F5;   map[GLFW_KEY_F6]  = KeyCode::F6;
        map[GLFW_KEY_F7]  = KeyCode::F7;   map[GLFW_KEY_F8]  = KeyCode::F8;
        map[GLFW_KEY_F9]  = KeyCode::F9;   map[GLFW_KEY_F10] = KeyCode::F10;
        map[GLFW_KEY_F11] = KeyCode::F11;  map[GLFW_KEY_F12] = KeyCode::F12;
        map[GLFW_KEY_F13] = KeyCode::F13;  map[GLFW_KEY_F14] = KeyCode::F14;
        map[GLFW_KEY_F15] = KeyCode::F15;  map[GLFW_KEY_F16] = KeyCode::F16;
        map[GLFW_KEY_F17] = KeyCode::F17;  map[GLFW_KEY_F18] = KeyCode::F18;
        map[GLFW_KEY_F19] = KeyCode::F19;  map[GLFW_KEY_F20] = KeyCode::F20;
        map[GLFW_KEY_F21] = KeyCode::F21;  map[GLFW_KEY_F22] = KeyCode::F22;
        map[GLFW_KEY_F23] = KeyCode::F23;  map[GLFW_KEY_F24] = KeyCode::F24;
        map[GLFW_KEY_F25] = KeyCode::F25;

        map[GLFW_KEY_KP_0]        = KeyCode::NumPad0;
        map[GLFW_KEY_KP_1]        = KeyCode::NumPad1;
        map[GLFW_KEY_KP_2]        = KeyCode::NumPad2;
        map[GLFW_KEY_KP_3]        = KeyCode::NumPad3;
        map[GLFW_KEY_KP_4]        = KeyCode::NumPad4;
        map[GLFW_KEY_KP_5]        = KeyCode::NumPad5;
        map[GLFW_KEY_KP_6]        = KeyCode::NumPad6;
        map[GLFW_KEY_KP_7]        = KeyCode::NumPad7;
        map[GLFW_KEY_KP_8]        = KeyCode::NumPad8;
        map[GLFW_KEY_KP_9]        = KeyCode::NumPad9;
        map[GLFW_KEY_KP_DECIMAL]  = KeyCode::NumPadDecimal;
        map[GLFW_KEY_KP_DIVIDE]   = KeyCode::NumPadDivide;
        map[GLFW_KEY_KP_MULTIPLY] = KeyCode::NumPadMultiply;
        map[GLFW_KEY_KP_SUBTRACT] = KeyCode::NumPadSubtract;
        map[GLFW_KEY_KP_ADD]      = KeyCode::NumPadAdd;
        map[GLFW_KEY_KP_ENTER]    = KeyCode::NumPadEnter;
        map[GLFW_KEY_KP_EQUAL]    = KeyCode::NumPadEqual;

        map[GLFW_KEY_LEFT_SHIFT]    = KeyCode::LeftShift;
        map[GLFW_KEY_LEFT_CONTROL]  = KeyCode::LeftControl;
        map[GLFW_KEY_LEFT_ALT]      = KeyCode::LeftAlt;
        map[GLFW_KEY_LEFT_SUPER]    = KeyCode::LeftSuper;
        map[GLFW_KEY_RIGHT_SHIFT]   = KeyCode::RightShift;
        map[GLFW_KEY_RIGHT_CONTROL] = KeyCode::RightControl;
        map[GLFW_KEY_RIGHT_ALT]     = KeyCode::RightAlt;
        map[GLFW_KEY_RIGHT_SUPER]   = KeyCode::RightSuper;
        map[GLFW_KEY_MENU]          = KeyCode::Menu;

        // Letters: GLFW lays them out as ASCII, so A..Z are contiguous — the same property our enum
        // guarantees for FirstLetter..LastLetter, which is what lets this be a loop instead of 26 lines.
        for (int letter = GLFW_KEY_A; letter <= GLFW_KEY_Z; ++letter)
        {
            map[static_cast<std::size_t>(letter)] =
                static_cast<KeyCode>(static_cast<UInt16>(KeyCode::FirstLetter) + (letter - GLFW_KEY_A));
        }

        // Digits: GLFW stores '0' at the ASCII value for '0' (48), i.e. *below* '1'. Our enum uses reading
        // order 1..9,0 so DigitKey(n) is a constant offset, so this loop cannot mirror the letter loop —
        // it maps each GLFW digit explicitly to our ordering.
        for (int digit = 1; digit <= 9; ++digit)
        {
            map[static_cast<std::size_t>(GLFW_KEY_0 + digit)] =
                static_cast<KeyCode>(static_cast<UInt16>(KeyCode::Digit1) + (digit - 1));
        }
        map[GLFW_KEY_0] = KeyCode::Digit0;
        // clang-format on

        return map;
    }();

    return table;
}

}  // namespace

KeyCode TranslateKey(int glfwKey) noexcept
{
    // GLFW_KEY_UNKNOWN is -1 and is delivered for keys it cannot map; negative indices must not reach the
    // table.
    if (glfwKey < 0 || glfwKey > kGlfwKeyRange)
    {
        return KeyCode::Unknown;
    }

    return BuildGlfwToEngineTable()[static_cast<std::size_t>(glfwKey)];
}

InputAction TranslateAction(int glfwAction) noexcept
{
    switch (glfwAction)
    {
        case GLFW_PRESS:
            return InputAction::Press;
        case GLFW_REPEAT:
            return InputAction::Repeat;
        case GLFW_RELEASE:
            return InputAction::Release;
        default:
            // A newer GLFW could add an action. Mapping it to Press would invent input, so callers drop
            // unknown actions by checking the translated value against the valid range.
            return InputAction::Release;
    }
}

KeyModifier TranslateModifiers(int glfwMods) noexcept
{
    KeyModifier modifiers = KeyModifier::None;
    if ((glfwMods & GLFW_MOD_SHIFT) != 0)
    {
        modifiers = modifiers | KeyModifier::Shift;
    }
    if ((glfwMods & GLFW_MOD_CONTROL) != 0)
    {
        modifiers = modifiers | KeyModifier::Control;
    }
    if ((glfwMods & GLFW_MOD_ALT) != 0)
    {
        modifiers = modifiers | KeyModifier::Alt;
    }
    if ((glfwMods & GLFW_MOD_SUPER) != 0)
    {
        modifiers = modifiers | KeyModifier::Super;
    }
    return modifiers;
}

MouseButton TranslateMouseButton(int glfwButton) noexcept
{
    switch (glfwButton)
    {
        case GLFW_MOUSE_BUTTON_LEFT:
            return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT:
            return MouseButton::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            return MouseButton::Middle;
        case GLFW_MOUSE_BUTTON_4:
            return MouseButton::Forward;
        case GLFW_MOUSE_BUTTON_5:
            return MouseButton::Back;
        case GLFW_MOUSE_BUTTON_6:
            return MouseButton::Extra1;
        case GLFW_MOUSE_BUTTON_7:
            return MouseButton::Extra2;
        case GLFW_MOUSE_BUTTON_8:
            return MouseButton::Extra3;
        default:
            return MouseButton::Left;
    }
}

char32_t DecodeUtf8CodePoint(const char* text) noexcept
{
    if (text == nullptr)
    {
        return U'\0';
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    const unsigned char lead = bytes[0];

    if (lead == 0)
    {
        return U'\0';
    }

    int remaining = 0;
    char32_t codePoint = 0;

    if ((lead & 0x80u) == 0x00u)
    {
        return static_cast<char32_t>(lead);
    }
    else if ((lead & 0xE0u) == 0xC0u)
    {
        codePoint = lead & 0x1Fu;
        remaining = 1;
    }
    else if ((lead & 0xF0u) == 0xE0u)
    {
        codePoint = lead & 0x0Fu;
        remaining = 2;
    }
    else if ((lead & 0xF8u) == 0xF0u)
    {
        codePoint = lead & 0x07u;
        remaining = 3;
    }
    else
    {
        return U'\uFFFD';  // A stray continuation byte, or a 5/6-byte form that is not valid UTF-8.
    }

    for (int index = 0; index < remaining; ++index)
    {
        const unsigned char continuation = bytes[index + 1];
        if ((continuation & 0xC0u) != 0x80u)
        {
            return U'\uFFFD';
        }
        codePoint = (codePoint << 6) | (continuation & 0x3Fu);
    }

    // Reject overlong encodings and surrogate halves. Accepting them would let one string decode
    // differently here than in every other decoder in the engine — an input-validation bug, not a
    // cosmetic one.
    static constexpr char32_t kMinimumForLength[4] = {0u, 0x80u, 0x800u, 0x10000u};
    if (codePoint < kMinimumForLength[remaining] || codePoint > 0x10FFFFu ||
        (codePoint >= 0xD800u && codePoint <= 0xDFFFu))
    {
        return U'\uFFFD';
    }

    return codePoint;
}

}  // namespace Hollowpoint::Platform::Detail
