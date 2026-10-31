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
/* sys/log.c's interrupt, records written: the system log's watchers look again. */
void stream_system_log_told(void);
/* The kernel's own line about an app (a fatal exception) to the app's error output, handle 2:
   written as bytes to a stream, posted as one message to a message channel, from the app's
   sender id. Nothing when it has none. */
struct process;
void stream_error_printf(struct process *p, const char *fmt, ...);

/* The keyboard's key events (dev/input.h's key sink) as the console's input, where no window
   manager takes them: characters, and the terminal's sequences for the moving keys. */
struct key_event;
void console_key(const struct key_event *e);
