/* Exhaustive 8-bit channel/weight equivalence, including independent packed
   lane extremes. Run on the host; no kernel timing or display claims. */
#include "../common/gfx.h"
#include <stdio.h>

static uint32_t reference(uint32_t a, uint32_t b, unsigned weight) {
    uint32_t result = 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8)
        result |= ((((a >> shift) & 255) * (255-weight) +
                    ((b >> shift) & 255) * weight + 127) / 255) << shift;
    return result;
}

int main(void) {
    for (unsigned a=0; a<=UINT8_MAX; a++)
        for (unsigned b=0; b<=UINT8_MAX; b++)
            for (unsigned weight=0; weight<=UINT8_MAX; weight++) {
                uint32_t left=RGB(a,a^0xa5,255-a), right=RGB(b,b^0x3c,255-b);
                if (gfx_mix(left,right,weight)!=reference(left,right,weight)) return 1;
                left=RGB(a,b,a);right=RGB(b,a,b);
                if (gfx_mix(left,right,weight)!=reference(left,right,weight)) return 1;
            }
    puts("gfxmix: PASS exhaustive channel/weight equivalence, 33554432 colors");
    return 0;
}
