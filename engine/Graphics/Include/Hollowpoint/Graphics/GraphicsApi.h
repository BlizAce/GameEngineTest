#pragma once

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"

// ---------------------------------------------------------------------------
// Common declarations for the Graphics module: export macro plumbing and the small value types shared by the
// device, resource and shader-compiler headers.
//
// This header deliberately includes no Vulkan and no NVRHI headers. Those appear only in Graphics/Src and in a
// private internal header, so that anything compiling against Hollowpoint/Graphics does not pay the compile-time
// cost of the Vulkan headers (which are enormous) and cannot accidentally start depending on RHI types where an
// engine-level abstraction was intended.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{

// Integer/size aliases only. Floating-point types come from Hollowpoint/Math, which this header deliberately does
// not include: pulling Math into every Graphics header would slow builds for a benefit these declarations do not
// need. Code that needs Float32 includes MathCore.h itself.
using Core::Size;
using Core::UInt16;
using Core::UInt32;
using Core::UInt64;
using Core::UInt8;

}  // namespace Hollowpoint::Graphics
