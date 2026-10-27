#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Graphics/GraphicsApi.h"
#include "Hollowpoint/Math/MathCore.h"
#include "Hollowpoint/Platform/InputEvent.h"
#include "Hollowpoint/Renderer/Camera.h"

// Forward declaration at Hollowpoint scope, before Runtime opens — the convention Graphics headers establish (a
// reference member needs a name, not a header; including RenderTarget.h here would drag Texture's and NVRHI's
// include graph into every consumer of this interface).
namespace Hollowpoint::Graphics
{
class RenderTarget;
}  // namespace Hollowpoint::Graphics

// ---------------------------------------------------------------------------
// IApplicationHost — the frame loop's control surface for an embedding host.
//
// RuntimeApplication was documented as embeddable ("the editor hosts this loop rather than duplicating it") and
// RunFrame() was public, but wiring the editor against it found three places where "designed for" stopped at the
// door. Each is a hook here:
//
//   1. Input. The loop polled window events and handed them to scripts. An embedding UI toolkit consumes events
//      (a keystroke typed into a text field is not also a gameplay key), and there was no way to say so — the same
//      keypress acted twice. ConsumeInputEvent gives the host first refusal, per event, in poll order.
//   2. Camera. The scene camera is chosen by scanning for the first active CameraComponent: correct for a game,
//      wrong for an editor viewport, where the live view is the user's fly camera and the scene camera is data
//      being edited. GetCameraOverride replaces that choice without touching the world.
//   3. Presentation. The loop renders its own plan into the HDR target and tone-maps it to the back buffer. An
//      editor composes a viewport panel plus docked UI, which needs the scene rendered at the *panel's* aspect into
//      a texture it can sample — not stretched across the window. RenderScenePass lets the host take over the frame
//      contents while the loop keeps owning acquire, the frame ring and present, which is the part no host should
//      reimplement.
//
// Layering: this header belongs to Runtime, not Editor. The alternative — Runtime including an editor type — would
// invert the dependency graph (AGENTS.md §3) and make every runtime build drag the editor along. The host supplies
// an implementation; Runtime knows only this interface, which it owns.
//
// Threading: every method is called on the thread running the frame loop, once per frame, in the order documented
// on RuntimeApplication's frame steps. Implementations may keep state between calls but must not block.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Runtime
{

using Core::UInt32;

//! What a host may control about one frame. Everything here is borrowed and valid only for the duration of the call:
//! the swapchain target is owned by the loop, and the world/scripts belong to the application.
struct FrameContext
{
    //! The acquired back buffer, i.e. what this frame must leave in it. Null when no image was acquired — a host
    //! called with null should return without drawing (the loop skips the frame).
    Graphics::RenderTarget* BackBuffer = nullptr;  // not owned

    //! Framebuffer-pixel extent of the back buffer. The swapchain's extent, which on a scaled display differs from
    //! the window's content size.
    UInt32 Width = 0;
    UInt32 Height = 0;

    //! This frame's ring slot, to be passed to any SceneRenderer call so per-frame constant buffers stay separated
    //! (the hazard FrameRing exists for).
    UInt32 Slot = 0;

    //! Scaled delta seconds from the application's TimeSystem — the same value scripts see, so a host animating UI or
    //! camera motion cannot drift from gameplay.
    double DeltaSeconds = 0.0;

    //! Frames completed before this one. Useful for "skip the first N frames" hosts and for cheap cadence control.
    Core::UInt64 FrameIndex = 0;
};

//! Implemented by an application that embeds the runtime loop (the editor, a harness). Every method has a default
//! reproducing plain standalone behaviour, so an implementation overrides only what it needs — which is also what
//! makes a partial host safe rather than silently wrong.
class HP_RUNTIME_API IApplicationHost
{
public:
    virtual ~IApplicationHost() = default;

    //! Called for each event drained from the window, in queue order, before scripts see it. Return true when the
    //! host consumed the event — scripts and gameplay will not receive it. Consuming must be per event rather than
    //! "swallow everything", because a host that ate resize/close events would break the loop itself (the loop acts
    //! on those regardless of this return value; see RuntimeApplication::RunFrame).
    virtual bool ConsumeInputEvent(const Platform::InputEvent& event)
    {
        static_cast<void>(event);
        return false;
    }

    //! Replaces the scene-camera scan when it returns true, filling a complete CameraState (the loop uses
    //! State.ViewProjection verbatim and EyePosition for view-dependent shading). Returning false keeps the normal
    //! "first active camera" rule.
    virtual bool GetCameraOverride(Renderer::CameraState& outCamera) const
    {
        static_cast<void>(outCamera);
        return false;
    }

    //! False skips the script tick (step 3 of the frame). An editor in edit mode wants Lua stopped — its scripts are
    //! gameplay, and a world that animates while you are placing things is not an editor. The loop keeps ticking
    //! transforms and rendering either way.
    virtual bool AreScriptsPaused() const
    {
        return false;
    }

    //! Takes the whole frame over when it returns true: the host produced every pixel of `context.BackBuffer` —
    //! scene, panels, widgets, all of it — in COLOR_ATTACHMENT_OPTIMAL layout (what Present's barrier expects). The
    //! loop then skips its own rendering and RenderOverlay is not called: one owner per frame, decided once.
    //!
    //! Taking over is how an editor renders at all: its scene is one rectangle of the composed frame (a viewport
    //! panel) with UI around it, which no overlay hook can express — an overlay draws across the whole window, not
    //! into a sub-rectangle. Returning false keeps the runtime's forward + tone-map pass and hands the frame to
    //! RenderOverlay instead; the two paths never mix, so "who drew this pixel" always has one answer.
    //!
    //! Whoever draws must clear: unwritten attachment memory is undefined and shows allocator garbage, which reads as
    //! a renderer bug rather than the missing clear it is (notes/EXPERIMENTS.md session 14).
    virtual bool RenderScenePass(const FrameContext& context)
    {
        static_cast<void>(context);
        return false;
    }

    //! Called when RenderScenePass declined the frame, after the runtime's own rendering and before present: the
    //! place for widgets drawn on top of a finished frame (a profiler overlay on a running game implements this and
    //! nothing else). The scene pixels exist, nothing has been presented, and the frame ring has not armed its
    //! completion query, so drawing now falls under the same "everything queued at EndFrame" contract.
    virtual void RenderOverlay(const FrameContext& context)
    {
        static_cast<void>(context);
    }

    //! Called when the window's framebuffer size changed, after the swapchain was rebuilt at the new extent. A host
    //! with viewport-sized render targets resizes its own allocations here.
    virtual void OnResize(UInt32 framebufferWidth, UInt32 framebufferHeight)
    {
        static_cast<void>(framebufferWidth);
        static_cast<void>(framebufferHeight);
    }
};

}  // namespace Hollowpoint::Runtime
