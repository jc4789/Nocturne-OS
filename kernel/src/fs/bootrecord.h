/* The boot record (common/settings_abi.h): the settings wanted before anything else runs. The boot
   loader brings /boot/settings.rec of the EFI system partition as the module "settings"; the kernel
   reads it first thing, and N_DOMAIN_BOOT's writes go back to that document on the disk. No record,
   or none that checks out: every value is its default. */
#pragma once
#include "kernel.h"
#include "settings_abi.h"

/* From kmain, before anything asks. */
void boot_record_read(void);
/* What this start goes by. */
int32_t boot_setting(enum n_boot_key key);

/* N_DOMAIN_BOOT's (sys/settings.h): value is an int32_t 0 or 1; it counts from the next start. */
int boot_setting_put(uint32_t key, const void *value);
int64_t boot_setting_get(uint32_t key, void *out, size_t cap);
