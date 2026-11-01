/* The platform's reference clocks (dev/refclock.c): what the clock (sys/clock.h) holds the TSC's
   rate and the wall clock to. Only the clock uses them.

   - The HPET's main counter (IA-PC HPET 1.0a), or where the platform has none the ACPI PM timer
     (ACPI 6.5 4.8.3.3): the clock's rate reference on the hardware (struct clock_rate_reference).
     Their owner here gives the clock its samples at the moments the clock asks for.
   - The CMOS RTC: the date the wall clock starts from and the second whose edge the clock finds
     (a wall-time reference), unless the FADT says there is none (IAPC_BOOT_ARCH bit 5, "CMOS RTC
     Not Present", ACPI 6.5 5.2.9.3). It counts whole seconds. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The rate reference, from clock_init (the ACPI tables up, before alarms come): found and seen
   counting, with nothing given to the clock yet. NULL where the platform has none that counts. */
struct clock_rate_reference;
const struct clock_rate_reference *refclock_rate_reference(void);

/* The RTC, any context but an NMI. rtc_read gives the date it holds as Unix seconds: RTC_READ
   when the reading is whole, RTC_UPDATING when it was updating (try again; the new second is
   not readable yet), RTC_ABSENT when there is none or what it holds is no date. */
enum rtc_result { RTC_ABSENT, RTC_UPDATING, RTC_READ };
enum rtc_result rtc_read(int64_t *unix_seconds);
