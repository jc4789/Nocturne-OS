/* Byte streams an app may be given as its input, output and log when no app is at the other
   end (docs/nocturne-native-abi.md §4): the text console, the system log and nothing. They are
   kernel objects, not names: nothing in the store stands for them. */
#pragma once
#include "kernel.h"

struct handle;

/* A new handle to the stream (NULL: no memory). */
struct handle *stream_console_open(void);
struct handle *stream_system_log_open(void);
struct handle *stream_null_open(void);

/* Keyboard input for the console (the text-mode keyboard, init.c). */
void console_input_char(char c);
void console_input_str(const char *s);
