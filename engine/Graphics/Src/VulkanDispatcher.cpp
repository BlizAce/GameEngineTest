#include "Hollowpoint/Graphics/RhiAccess.h"

// ---------------------------------------------------------------------------
// The program's single definition of the Vulkan-Hpp dynamic dispatcher.
//
// NVRHI's Vulkan backend calls every Vulkan entry point through `vk::detail::defaultDispatchLoaderDynamic`. It
// defines that object itself only when built as a shared library (guarded by NVRHI_SHARED_LIBRARY_BUILD); we link
// NVRHI statically, so libnvrhi_vk.a references it as an undefined symbol and no definition exists anywhere unless
// a translation unit in this program provides one. `nm -C libnvrhi_vk.a | grep defaultDispatchLoaderDynamic` shows
// only 'U' entries — that is what this file satisfies.
//
// Exactly one such definition may exist in a program: more than one is an ODR violation whose symptom is a call
// through an uninitialised function-pointer table (a null-pointer crash inside the first vkCmd* of the first frame),
// which is why it lives here rather than in the header every RHI user includes.
// ---------------------------------------------------------------------------

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
