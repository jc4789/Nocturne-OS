#pragma once
/* Nocturne's own stand-in for Mesa's include/drm-uapi/drm_fourcc.h.

   Nocturne has no DRM and takes no Linux headers. Vulkan's VK_EXT_image_drm_format_modifier names
   a tiling layout with a 64-bit "format modifier" whose top byte is a vendor id; that numbering is
   a Khronos/industry registry and this file carries only the few registry values the Vulkan
   runtime and NVK compare against (linear, invalid, the vendor byte of NVIDIA's modifiers). The
   modifier of an image never names a Linux object: it only says how the texels are laid out. */
#include <stdint.h>

#define DRM_FORMAT_MOD_VENDOR_NONE   0
#define DRM_FORMAT_MOD_VENDOR_NVIDIA 0x03

#define DRM_FORMAT_RESERVED          ((1ULL << 56) - 1)
#define DRM_FORMAT_MOD_INVALID       DRM_FORMAT_RESERVED
#define DRM_FORMAT_MOD_LINEAR        0ULL

#define fourcc_mod_is_vendor(modifier, vendor) \
    (((uint64_t)(modifier) >> 56) == DRM_FORMAT_MOD_VENDOR_##vendor)
