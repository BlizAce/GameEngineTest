#pragma once

#include <string>
#include <vector>

#include "Hollowpoint/Core/API.h"
#include "Hollowpoint/Core/Types.h"
#include "Hollowpoint/Graphics/GraphicsTypes.h"

// ---------------------------------------------------------------------------
// Screenshots: read a rendered target back to CPU and write it as a PNG.
//
// Why this lives in the runtime rather than Graphics. Graphics owns textures and knows how to copy one to a staging
// buffer; deciding what a "screenshot" *is* — which format, whether tone mapping has been applied, what row order a
// viewer expects — is an application decision. Keeping it here also keeps stb_image_write out of the graphics layer's
// compile graph.
//
// Why the engine needs one at all: every other observable this project has is numeric. A PNG is the only artifact that
// shows whether a frame looks right rather than merely finite, and the difference matters — an image can pass every
// texel assertion in the suite and still be upside down, lit from the wrong side, or rendering only back faces. It is
// also what makes an unattended run reviewable by a human: `--screenshot out.png --frames 3` turns the engine into
// something that can be checked without a display, which on this host (Wayland session, no window manager guarantee) is
// the difference between testable and not.
//
// Row order: rows are written in readback order, i.e. the same row index Texture::Update would upload them at, so a
// screenshot of a texture round-trips with the bytes that were uploaded into it. Whether that reads as "upright" for a
// rendered view is a property of the projection's Y convention, which no test pins yet — stated rather than assumed,
// because a flipped PNG looks exactly like a correctly-saved one until someone opens it.
// ---------------------------------------------------------------------------

namespace Hollowpoint::Graphics
{
class GraphicsDevice;
class Texture;
}  // namespace Hollowpoint::Graphics

namespace Hollowpoint::Runtime
{

//! Converts readback bytes of `format` into 8-bit RGBA, applying no colour transform.
/**
 * Supported source formats are the ones the engine renders into: Rgba32Float and Rgba16Float (the HDR target), and
 * Bgra8Unorm/Rgba8Unorm (a swapchain back buffer). Float inputs are clamped to [0,1] — a value above 1 means the tone
 * map was skipped, which is a bug in the caller rather than something to encode here, and clamping keeps the output
 * viewable.
 *
 * Returns false with `outError` naming the problem when the format has no conversion or the byte count does not match
 * an extent/rowPitch combination. A short buffer means the readback itself went wrong; reporting it as "0 pixels" would
 * let a blank image through as if it were a successful capture.
 */
[[nodiscard]] bool ConvertToRgba8(const void* source, Core::Size byteCount, Core::UInt32 rowPitchBytes,
                                  Graphics::GraphicsFormat format, Core::UInt32 width, Core::UInt32 height,
                                  std::vector<unsigned char>& outRgba8, std::string& outError);

//! Writes `texture`'s current contents to `path` as an 8-bit RGBA PNG.
/**
 * Blocks on the GPU (ReadBack does), so call it when the frame you want is complete — after WaitForIdle in a test, or
 * at the end of a bounded run. Not something to call every frame: a readback stalls the pipeline by design.
 *
 * `texture` is non-const because Texture::ReadBack is: it allocates a staging texture and transitions the source's
 * resource state. Taking const here and casting it away would advertise an operation that does not mutate anything
 * while doing both of those things, so the constness matches what happens.
 *
 * Returns false with a message when the texture cannot be read back (no staging memory, unsupported format) or the file
 * cannot be written (`path` in a directory that does not exist is the common one, and stb reports that only as "could
 * not open", so the parent directory is checked first to say something useful).
 */
[[nodiscard]] bool WriteScreenshot(Graphics::GraphicsDevice& device, Graphics::Texture& texture,
                                   const std::string& path, std::string& outError);

}  // namespace Hollowpoint::Runtime
