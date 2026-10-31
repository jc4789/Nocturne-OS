/* The platform's reference clocks (dev/refclock.c): what the clock (sys/clock.h) holds the TSC's
   rate and the wall clock to. Only the clock uses them.

   - The HPET's main counter (IA-PC HPET 1.0a): the clock's rate reference on the hardware (struct
     clock_rate_reference). It is read in one memory access, and its owner here gives the clock its
     samples at the moments the clock asks for.
   - The CMOS RTC: the date the wall clock starts from and the second whose edge the clock finds
     (a wall-time reference), unless the FADT says there is none (IAPC_BOOT_ARCH bit 5, "CMOS RTC
     Not Present", ACPI 6.5 5.2.9.3). It counts whole seconds. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The HPET as the clock's rate reference, from clock_init on the hardware (the ACPI tables up,
   before alarms come): found, its counter on, its first sample given to the clock and its alarm
   set for the next. False where there is none, or none valid. */
bool refclock_hpet_start(void);

/* The RTC, any context but an NMI. rtc_read gives the date it holds as Unix seconds: RTC_READ
   when the reading is whole, RTC_UPDATING when it was updating (try again; the new second is
   not readable yet), RTC_ABSENT when there is none or what it holds is no date. */
enum rtc_result { RTC_ABSENT, RTC_UPDATING, RTC_READ };
enum rtc_result rtc_read(int64_t *unix_seconds);
