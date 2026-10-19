#pragma once

// ---------------------------------------------------------------------------
// Include NVRHI (and the Vulkan/Vulkan-Hpp headers it requires) from C++ through this file, never directly.
//
// Two reasons, both learned by being broken:
//
//   1. Warnings. NVRHI's headers declare locals named `hash` inside std::hash specialisations and use old-style
//      casts, which fail our -Wshadow / -Wold-style-cast under -Werror. Suppressed here so engine code below the
//      includes is still fully checked, and so a new translation unit cannot reintroduce the failure by including
//      <nvrhi/nvrhi.h> directly.
//
//   2. The Vulkan-Hpp dispatcher. NVRHI's Vulkan backend calls through the dynamic dispatcher and defines its
//      storage only when built as a shared library. We link it statically, so libnvrhi_vk.a leaves
//      vk::detail::defaultDispatchLoaderDynamic undefined (verified with nm) and exactly one translation unit in
//      this program must define it — see VulkanDispatcher.cpp, which includes this header for that purpose. Getting
//      this wrong is not a link error in the module: it appears when a final executable links deep RHI calls, as an
//      undefined symbol or (if defined twice) an ODR violation.
// ---------------------------------------------------------------------------

// Users of the RHI need the device type as well as nvrhi::IDevice, so this header provides both — including it to
// reach the RHI should never require guessing which second header to pair it with. Included before the warning
// suppressions below so engine headers are still compiled with full checking.
#include "Hollowpoint/Graphics/GraphicsDevice.h"

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wshadow"
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wpedantic"
    #pragma clang diagnostic ignored "-Wextra-semi"
    #pragma clang diagnostic ignored "-Wcast-qual"
    #pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
    #pragma clang diagnostic ignored "-Wcomma"
    #pragma clang diagnostic ignored "-Wc++98-compat-extra-semi"
#elif defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wshadow"
    #pragma GCC diagnostic ignored "-Wold-style-cast"
    #pragma GCC diagnostic ignored "-Wpedantic"
    #pragma GCC diagnostic ignored "-Wextra-semi"
    #pragma GCC diagnostic ignored "-Wcast-qual"
    #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif

// Request the dynamic loader before including Vulkan-Hpp: the macro selects which dispatcher object the generated
// inline functions call, and it must be set consistently in every translation unit that sees Vulkan.hpp. Mixing a
// static-dispatch TU with dynamic-dispatch ones links but calls through an uninitialised table — i.e. a null-pointer
// crash at the first vkCmd* call.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <nvrhi/nvrhi.h>
#include <nvrhi/utils.h>
#include <nvrhi/vulkan.h>

#include <vulkan/vulkan.hpp>

#if defined(__clang__) || defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
