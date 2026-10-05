/* Prints a random bit of night-time wisdom */
#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"

static const char *f[] = {
    "The moon does not fight. It attacks no one. It does not worry.\nIt simply shines.",
    "There are 10 kinds of people: those who understand binary and those who don't.",
    "A kernel panic is just the computer's way of asking for a nap.",
    "It works on my VM.",
    "Real programmers count from 0.",
    "Sleep is a great debugger. Most bugs look smaller in the morning.",
    "To understand recursion, you must first understand recursion.",
    "The night is darker just before the coffee.",
    "Every operating system is a toy until somebody depends on it.",
    "Somewhere, a page fault is happening for a very good reason.",
    "Have you tried turning it off and on again? (type: reboot)",
    "Keep your stack aligned and your heap tidy.",
    "In the beginning there was nothing, which exploded. Then came the bootloader.",
    "The best code is no code at all. The second best is code that compiles.",
    "Owls are just night-mode birds.",
    "One does not simply write an operating system... except tonight.",
    "Ctrl+C is the sound of a thousand processes sighing in relief.",
    "If at first you don't succeed, call it version 0.1.",
    "Stars are just the universe's status LEDs.",
    "All your interrupts are belong to us.",
};

int main(void) {
    srand((unsigned)(uptime_ms() * 2654435761u));
    puts(f[rand() % (int)(sizeof f / sizeof f[0])]);
    return 0;
}
