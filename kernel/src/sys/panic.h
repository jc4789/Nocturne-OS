/* A panic's report (sys/panic.c) as lines, for the two places it goes straight to: the screen shown
   (dev/fbcon.c) and the serial port (dev/serial.c). The first line is the message. */
#pragma once

enum panic_tone { PANIC_MESSAGE, PANIC_HEADING, PANIC_DETAIL };
struct panic_line {
    const char *text;
    enum panic_tone tone;
};
