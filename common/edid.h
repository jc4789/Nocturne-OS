#pragma once
/* A monitor's EDID (VESA E-EDID 1.3/1.4, the 128-byte base block): what the monitor says it is
   and which modes its timings name. The kernel reads the one the firmware read from the monitor
   and passed with its framebuffer (dev/fb.c); nvrm the one NVKMS read over the display's link
   (ports/nvidia/display.c). Both know a monitor by edid_monitor_id. No allocation, no library. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDID_BLOCK 128u

struct edid_ids {
    char maker[4];    /* the manufacturer's three-letter id */
    unsigned product; /* its product code */
    uint32_t serial;
    char name[14];    /* the monitor name descriptor's text, "" without one */
};

/* A mode a timing names. refresh_mhz: a detailed timing's own, a standard or established timing's
   nominal rate. preferred: the first detailed timing, the monitor's native mode. */
struct edid_mode {
    uint32_t width, height, refresh_mhz;
    bool detailed, preferred;
};

/* The header and the checksum of the base block. */
static inline bool edid_valid(const uint8_t *e, size_t n) {
    static const uint8_t header[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    if (!e || n < EDID_BLOCK) return false;
    uint8_t sum = 0;
    for (unsigned i = 0; i < EDID_BLOCK; i++) {
        if (i < sizeof header && e[i] != header[i]) return false;
        sum = (uint8_t)(sum + e[i]);
    }
    return sum == 0;
}

static inline bool edid_ids(const uint8_t *e, size_t n, struct edid_ids *id) {
    if (!e || n < EDID_BLOCK) return false;
    unsigned m = (unsigned)e[8] << 8 | e[9];
    id->maker[0] = (char)('@' + (m >> 10 & 31));
    id->maker[1] = (char)('@' + (m >> 5 & 31));
    id->maker[2] = (char)('@' + (m & 31));
    id->maker[3] = 0;
    id->product = (unsigned)e[10] | (unsigned)e[11] << 8;
    id->serial = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
    id->name[0] = 0;
    for (unsigned d = 54; d + 18 <= 126; d += 18) {
        const uint8_t *b = e + d;
        if (b[0] || b[1] || b[2] || b[3] != 0xfc) continue;
        size_t k = 0;
        for (unsigned j = 5; j < 18 && b[j] != 0x0a; j++) id->name[k++] = b[j] >= 0x20 && b[j] < 0x7f ? (char)b[j] : '?';
        while (k && id->name[k - 1] == ' ') k--;
        id->name[k] = 0;
        break;
    }
    return true;
}

/* What the system knows a monitor by (struct n_screen_modes' monitor, the user's choice of mode is
   per monitor): maker, product code and serial number, "HPN375b-01010101". */
#define EDID_MONITOR_ID_SIZE 17u
static inline void edid_monitor_id(const struct edid_ids *id, char out[EDID_MONITOR_ID_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 3; i++) out[i] = id->maker[i];
    for (unsigned i = 0; i < 4; i++) out[3 + i] = hex[id->product >> (12 - 4 * i) & 15];
    out[7] = '-';
    for (unsigned i = 0; i < 8; i++) out[8 + i] = hex[id->serial >> (28 - 4 * i) & 15];
    out[16] = 0;
}

/* The modes the base block's timings name, at most cap: its detailed timings (progressive ones),
   then its standard timings, then its established ones. Returns how many. */
static inline unsigned edid_modes(const uint8_t *e, struct edid_mode *out, unsigned cap) {
    static const struct { uint16_t width, height; uint8_t hz, byte, bit; } established[] = {
        {720, 400, 70, 35, 7}, {720, 400, 88, 35, 6}, {640, 480, 60, 35, 5}, {640, 480, 67, 35, 4},
        {640, 480, 72, 35, 3}, {640, 480, 75, 35, 2}, {800, 600, 56, 35, 1}, {800, 600, 60, 35, 0},
        {800, 600, 72, 36, 7}, {800, 600, 75, 36, 6}, {832, 624, 75, 36, 5}, {1024, 768, 60, 36, 3},
        {1024, 768, 70, 36, 2}, {1024, 768, 75, 36, 1}, {1280, 1024, 75, 36, 0}, {1152, 870, 75, 37, 7},
    };
    unsigned n = 0;
    bool first = true;
    for (unsigned d = 54; d + 18 <= 126 && n < cap; d += 18) {
        const uint8_t *t = e + d;
        uint32_t clock = (uint32_t)t[0] | (uint32_t)t[1] << 8; /* 10 kHz; 0: a descriptor */
        if (!clock) continue;
        uint32_t w = t[2] | (uint32_t)(t[4] & 0xf0) << 4, hblank = t[3] | (uint32_t)(t[4] & 0x0f) << 8;
        uint32_t h = t[5] | (uint32_t)(t[7] & 0xf0) << 4, vblank = t[6] | (uint32_t)(t[7] & 0x0f) << 8;
        uint64_t total = (uint64_t)(w + hblank) * (h + vblank);
        bool preferred = first;
        first = false;
        if (!w || !h || (t[17] & 0x80)) continue; /* interlaced */
        out[n++] = (struct edid_mode){w, h, (uint32_t)(((uint64_t)clock * 10000000 + total / 2) / total), true, preferred};
    }
    for (unsigned s = 38; s < 54 && n < cap; s += 2) {
        if (e[s] <= 1) continue; /* unused: 0x01 0x01 */
        uint32_t w = (e[s] + 31u) * 8, ratio = e[s + 1] >> 6;
        uint32_t h = ratio == 0 ? (e[18] == 1 && e[19] < 3 ? w : w * 10 / 16) /* 1:1 before EDID 1.3 */
                   : ratio == 1 ? w * 3 / 4 : ratio == 2 ? w * 4 / 5 : w * 9 / 16;
        out[n++] = (struct edid_mode){w, h, ((e[s + 1] & 63u) + 60) * 1000, false, false};
    }
    for (unsigned i = 0; i < sizeof established / sizeof established[0] && n < cap; i++)
        if (e[established[i].byte] >> established[i].bit & 1)
            out[n++] = (struct edid_mode){established[i].width, established[i].height, established[i].hz * 1000u, false, false};
    return n;
}
