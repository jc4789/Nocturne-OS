/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 *
 * The chip-derived part of struct nv_device_info.  These tables were
 * nouveau_device.c in Mesa's DRM winsys (src/nouveau/winsys); they know
 * nothing about how a kernel driver is reached, so every nvkmd backend shares
 * them.  A backend reads the raw numbers from its driver (chipset, unit
 * counts, classes, memory sizes) and calls nvkmd_device_info_finish().
 */
#ifndef NVKMD_DEVICE_INFO_H
#define NVKMD_DEVICE_INFO_H 1

#include "nv_device_info.h"

#include <stdint.h>

/** Shader model for a chipset (0 when the chip has no compute) */
uint8_t nvkmd_sm_for_chipset(uint16_t chipset);

/** Marketing name for a PCI device, or NULL when the table has none
 *
 * (subsystem_id, subsystem_vendor_id) pick a board-specific name when the
 * table has one; the chip's default name is the fallback.
 */
const char *nvkmd_name_for_chip(uint16_t device_id,
                                uint16_t subsystem_id,
                                uint16_t subsystem_vendor_id);

/** Fill in everything that follows from info->chipset and info->sm
 *
 * Needs info->chipset set first.  Sets sm, max_warps_per_mp,
 * max_blocks_per_mp, mp_per_tpc and the shared memory configuration.
 */
void nvkmd_device_info_finish(struct nv_device_info *info);

#endif /* NVKMD_DEVICE_INFO_H */
