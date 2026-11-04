/* The remote desktop server for Hyper-V's enhanced session (rdp.c). */
#pragma once

/* Listen on the Hyper-V socket port the host connects to. Needs VMBus (vmbus_ready). */
void rdp_init(void);
