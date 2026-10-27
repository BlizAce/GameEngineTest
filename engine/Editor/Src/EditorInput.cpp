#include "Hollowpoint/Core/Log.h"
#include "Hollowpoint/Editor/EditorInput.h"

#include <imgui.h>

// ---------------------------------------------------------------------------
// See the header for why the engine maps its own events rather than using imgui_impl_glfw.
//
// Structure: one switch translating KeyCode → ImGuiKey (returning ImGuiKey_None when unmapped, never a guess), plus
// small handlers per event type. Everything funnels into ImGui's queued-event API so ordering is preserved and no
// global "current state" is consulted — the same argument Platform/InputEvent.h makes for its own queue.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Editor
{

namespace
{

using Platform::InputAction;
using Platform::InputEvent;
using Platform::KeyCode;
using Platform::KeyModifier;
using Platform::MouseButton;

[[nodiscard]] inline bool IsDown(InputAction action) noexcept
{
    return action != InputAction::Release;
}

//! ImGui's modifier flags for an engine modifier set.
/**
 * Modifiers are submitted as the synthetic ImGuiMod_* keys, which is what every stock backend does (imgui_impl_glfw
 * calls io.AddKeyEvent(ImGuiMod_Ctrl, ...) from a polled state read). Doing it per event rather than by polling the
 * window keeps this module free of Platform::Window and means a scripted event stream reproduces chords exactly: an
 * injected Ctrl+A carries Modifiers=Control on the A event, so the chord is true for that event and false afterwards.
 */
void FeedModifiers(KeyModifier modifiers)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, HasModifier(modifiers, KeyModifier::Control));
    io.AddKeyEvent(ImGuiMod_Shift, HasModifier(modifiers, KeyModifier::Shift));
    io.AddKeyEvent(ImGuiMod_Alt, HasModifier(modifiers, KeyModifier::Alt));
    io.AddKeyEvent(ImGuiMod_Super, HasModifier(modifiers, KeyModifier::Super));
}

//! ImGui's mouse button index for an engine button.
/**
 * Not identity: engine Left/Right/Middle are 0/1/2 while ImGui expects 0/1/2 as left/right/middle — which happens to
 * match — but Forward/Back are 3/4 in both and ImGui's AddMouseButtonEvent accepts those directly, so the mapping is a
 * range check rather than a table. Extra1..Extra4 have no ImGui binding at all and are dropped: ImGui exposes no
 * widget for them, so feeding them would update state nothing reads (and "state nothing reads" is how a stale button
 * ends up blocking a drag).
 */
[[nodiscard]] constexpr int ImGuiButtonFor(MouseButton button) noexcept
{
    switch (button)
    {
        case MouseButton::Left:
            return 0;
        case MouseButton::Right:
            return 1;
        case MouseButton::Middle:
            return 2;
        case MouseButton::Forward:
            return 3;
        case MouseButton::Back:
            return 4;
        default:
            return -1;
    }
}

}  // namespace

int ImGuiKeyFor(KeyCode key) noexcept
{
    // Ranges that are contiguous in both enums are arithmetic, with the offset asserted at compile time below rather
    // than trusted: a renumbering of either enum (ImGui adds keys between releases; the engine reserves ranges for
    // future ones) would otherwise silently shift every letter by one, i.e. "W moves the camera left".
    if (key >= KeyCode::FirstLetter && key <= KeyCode::LastLetter)
    {
        static_assert(static_cast<int>(ImGuiKey_B) - static_cast<int>(ImGuiKey_A) == 1,
                      "ImGui letter keys are no longer contiguous");
        return static_cast<int>(ImGuiKey_A) + (static_cast<int>(key) - static_cast<int>(KeyCode::FirstLetter));
    }

    // Engine top-row digits are stored 1..9 then 0 (reading order, see DigitKey), ImGui's are 0..9. So a linear offset
    // is wrong for exactly one input and would map Digit0 to "1" — the kind of bug that hides in every test that only
    // presses letters until someone types a number into an entity name.
    if (key >= KeyCode::Digit1 && key <= KeyCode::Digit0)
    {
        const int digit = key == KeyCode::Digit0 ? 0 : static_cast<int>(key) - static_cast<int>(KeyCode::Digit1) + 1;
        return static_cast<int>(ImGuiKey_0) + digit;
    }

    if (key >= KeyCode::F1 && key <= KeyCode::F25)
    {
        // ImGui defines F1..F24; F25 has no equivalent and reports unmapped rather than aliasing to F24, because an
        // alias makes a binding for F25 fire the action assigned to F24.
        if (key > KeyCode::F24)
        {
            return 0;
        }
        static_assert(static_cast<int>(ImGuiKey_F2) - static_cast<int>(ImGuiKey_F1) == 1,
                      "ImGui function keys are no longer contiguous");
        return static_cast<int>(ImGuiKey_F1) + (static_cast<int>(key) - static_cast<int>(KeyCode::F1));
    }

    if (key >= KeyCode::NumPad0 && key <= KeyCode::NumPad9)
    {
        // Engine keypad digits are 0..9 in declaration order, matching ImGui's. Asserted the same way as above.
        return static_cast<int>(ImGuiKey_Keypad0) + (static_cast<int>(key) - static_cast<int>(KeyCode::NumPad0));
    }

    switch (key)
    {
        case KeyCode::Space:
            return ImGuiKey_Space;
        case KeyCode::Apostrophe:
            return ImGuiKey_Apostrophe;
        case KeyCode::Comma:
            return ImGuiKey_Comma;
        case KeyCode::Minus:
            return ImGuiKey_Minus;
        case KeyCode::Period:
            return ImGuiKey_Period;
        case KeyCode::Slash:
            return ImGuiKey_Slash;
        case KeyCode::Semicolon:
            return ImGuiKey_Semicolon;
        case KeyCode::Equal:
            return ImGuiKey_Equal;
        case KeyCode::LeftBracket:
            return ImGuiKey_LeftBracket;
        case KeyCode::Backslash:
            return ImGuiKey_Backslash;
        case KeyCode::RightBracket:
            return ImGuiKey_RightBracket;
        case KeyCode::GraveAccent:
            return ImGuiKey_GraveAccent;
        case KeyCode::Escape:
            return ImGuiKey_Escape;
        case KeyCode::Enter:
            return ImGuiKey_Enter;
        case KeyCode::Tab:
            return ImGuiKey_Tab;
        case KeyCode::Backspace:
            return ImGuiKey_Backspace;
        case KeyCode::Insert:
            return ImGuiKey_Insert;
        case KeyCode::Delete:
            return ImGuiKey_Delete;
        case KeyCode::Right:
            return ImGuiKey_RightArrow;
        case KeyCode::Left:
            return ImGuiKey_LeftArrow;
        case KeyCode::Down:
            return ImGuiKey_DownArrow;
        case KeyCode::Up:
            return ImGuiKey_UpArrow;
        case KeyCode::PageUp:
            return ImGuiKey_PageUp;
        case KeyCode::PageDown:
            return ImGuiKey_PageDown;
        case KeyCode::Home:
            return ImGuiKey_Home;
        case KeyCode::End:
            return ImGuiKey_End;
        case KeyCode::CapsLock:
            return ImGuiKey_CapsLock;
        case KeyCode::ScrollLock:
            return ImGuiKey_ScrollLock;
        case KeyCode::NumLock:
            return ImGuiKey_NumLock;
        case KeyCode::PrintScreen:
            return ImGuiKey_PrintScreen;
        case KeyCode::Pause:
            return ImGuiKey_Pause;
        case KeyCode::NumPadDecimal:
            return ImGuiKey_KeypadDecimal;
        case KeyCode::NumPadDivide:
            return ImGuiKey_KeypadDivide;
        case KeyCode::NumPadMultiply:
            return ImGuiKey_KeypadMultiply;
        case KeyCode::NumPadSubtract:
            return ImGuiKey_KeypadSubtract;
        case KeyCode::NumPadAdd:
            return ImGuiKey_KeypadAdd;
        case KeyCode::NumPadEnter:
            return ImGuiKey_KeypadEnter;
        case KeyCode::NumPadEqual:
            return ImGuiKey_KeypadEqual;
        case KeyCode::LeftShift:
            return ImGuiKey_LeftShift;
        case KeyCode::LeftControl:
            return ImGuiKey_LeftCtrl;
        case KeyCode::LeftAlt:
            return ImGuiKey_LeftAlt;
        case KeyCode::LeftSuper:
            return ImGuiKey_LeftSuper;
        case KeyCode::RightShift:
            return ImGuiKey_RightShift;
        case KeyCode::RightControl:
            return ImGuiKey_RightCtrl;
        case KeyCode::RightAlt:
            return ImGuiKey_RightAlt;
        case KeyCode::RightSuper:
            return ImGuiKey_RightSuper;
        case KeyCode::Menu:
            return ImGuiKey_Menu;

        // World1/World2 are OS-dependent scancodes (often the international backslash); ImGui's Oem102 is the closest
        // but its meaning varies by layout, so mapping it would be a guess. Unmapped = the key simply does nothing in
        // widgets.
        case KeyCode::World1:
        case KeyCode::World2:
        default:
            return 0;  // ImGuiKey_None
    }
}

bool HasImGuiKeyMapping(KeyCode key) noexcept
{
    return ImGuiKeyFor(key) != 0;
}

Index FeedImGuiInput(const std::vector<InputEvent>& events)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        HP_LOG_ERROR("EDITOR", "FeedImGuiInput called with no ImGui context");
        return 0;
    }

    ImGuiIO& io = ImGui::GetIO();
    Index translated = 0;

    for (const InputEvent& event : events)
    {
        switch (event.EventType)
        {
            case InputEvent::Type::Key:
            {
                // Modifiers first, so a chord's key event is processed with its modifiers already applied. Within one
                // frame ImGui resolves state at NewFrame, but ordering it this way means the sequence is also correct
                // if anything reads io.KeyCtrl between these calls (a debug overlay does).
                FeedModifiers(event.Modifiers);

                const int imguiKey = ImGuiKeyFor(event.Key);
                if (imguiKey == 0)
                {
                    continue;  // unmapped: nothing to feed, and not counted as translated
                }
                io.AddKeyEvent(static_cast<ImGuiKey>(imguiKey), IsDown(event.Action));

                // ImGui distinguishes a held key from an auto-repeat for exactly one purpose (its own repeat handling
                // in text fields). AddKeyEvent has no repeat flag; the stock backends submit Press and Release only,
                // and a Repeat event arrives as a down-state refresh, which is what feeding it as "down" produces.
                // Noted because the engine enum *does* carry the distinction and a reader will look for where it went:
                // it stays in the engine's queue (GetUnconsumedEvents) for gameplay bindings, which do care.
                ++translated;
                break;
            }

            case InputEvent::Type::Character:
            {
                if (event.Character == U'\0')
                {
                    continue;
                }
                // Code points go through untouched — that is the entire point of Character events existing (Platform's
                // header explains why key-derived text is wrong on non-US layouts). ImGui takes them as unsigned ints.
                io.AddInputCharacter(static_cast<unsigned int>(event.Character));
                ++translated;
                break;
            }

            case InputEvent::Type::MouseButton:
            {
                FeedModifiers(event.Modifiers);
                const int button = ImGuiButtonFor(event.Button);
                if (button < 0)
                {
                    continue;
                }
                io.AddMouseButtonEvent(button, IsDown(event.Action));
                ++translated;
                break;
            }

            case InputEvent::Type::MouseMove:
            {
                // Content coordinates, the same space io.DisplaySize is set to in FeedImGuiFrameState. Mixing in
                // framebuffer pixels here would offset every widget by the DPI scale factor on HiDPI displays only,
                // which is a bug report nobody can reproduce on their own laptop.
                io.AddMousePosEvent(event.Position.X, event.Position.Y);
                ++translated;
                break;
            }

            case InputEvent::Type::Scroll:
            {
                // Engine scroll is "lines, sign-correct" with positive Y meaning the same direction ImGui's wheel-up
                // means (verified against GLFW's callback sign in Platform/Src); ImGui wants wheel_y>0 for up. Feeding
                // the values straight through is therefore correct — and it is the one place where a sign flip would be
                // invisible in a screenshot and obvious only while scrolling a long outliner the wrong way.
                io.AddMouseWheelEvent(event.ScrollX, event.ScrollY);
                ++translated;
                break;
            }

            case InputEvent::Type::Focus:
            {
                io.AddFocusEvent(event.Focused);
                ++translated;
                break;
            }

            // Resize/Iconify are the loop's business (it rebuilds the swapchain) and DropFile is handled by the
            // editor's own command path, not by widgets. Not fed to ImGui: it has no event for them and infers size
            // changes from io.DisplaySize, which FeedImGuiFrameState updates every frame.
            case InputEvent::Type::WindowResize:
            case InputEvent::Type::Iconify:
            case InputEvent::Type::CloseRequested:
            case InputEvent::Type::DropFile:
            default:
                break;
        }
    }

    return translated;
}

void FeedImGuiFrameState(float contentWidth, float contentHeight, float deltaSeconds, bool hasFocus)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        HP_LOG_ERROR("EDITOR", "FeedImGuiFrameState called with no ImGui context");
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Clamped to 1: a zero extent reaches the vertex shader as a divisor (the UI shader divides by ScreenSize), and a
    // negative one from a bogus caller produces inverted geometry. A window mid-iconify reports 0x0 on some
    // compositors, so this is not defensive coding for an impossible case.
    io.DisplaySize.x = contentWidth > 1.0f ? contentWidth : 1.0f;
    io.DisplaySize.y = contentHeight > 1.0f ? contentHeight : 1.0f;
    io.DeltaTime = deltaSeconds > 0.0f ? deltaSeconds : 1.0f / 60.0f;

    // Focus is also an event (see the Focus case above), and both paths write the same state. That is intentional:
    // the event tells ImGui *when* focus changed (which drives its click-through behaviour on refocus), while this
    // guarantees the state is right even when the change happened before the editor attached a host — e.g. an editor
    // opened behind another window starts unfocused and gains it via an event that was never queued for us.
    io.AddFocusEvent(hasFocus);
}

}  // namespace Hollowpoint::Editor
