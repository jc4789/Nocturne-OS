/* DNS representation bounds, not browser hostname quotas. RFC 1035 2.3.4:
   labels <=63 octets, name including label lengths/root <=255 wire octets.
   The longest unescaped textual form has 254 bytes including its final dot. */
#pragma once
#define DNS_NAME_WIRE_BYTES 255u
#define DNS_NAME_TEXT_BYTES 254u

static bool dns_name_encode(const char *name, size_t length, uint8_t *out, size_t capacity, size_t *written) {
    if (!name || !out || !written) return false;
    for (size_t i = 0; i < length; i++) if (!name[i]) return false;
    size_t at = 0, start = 0;
    if (length == 1 && name[0] == '.') length = 0; /* root */
    while (start < length) {
        size_t stop = start;
        while (stop < length && name[stop] != '.') stop++;
        size_t label = stop - start;
        if (!label || label > 63 || at >= DNS_NAME_WIRE_BYTES ||
            label + 1 > DNS_NAME_WIRE_BYTES - at - 1 ||
            at >= capacity || label + 1 > capacity - at - 1) return false;
        out[at++] = (uint8_t)label;
        memcpy(out + at, name + start, label); at += label;
        start = stop < length ? stop + 1 : stop;
    }
    if (at >= capacity || at >= DNS_NAME_WIRE_BYTES) return false;
    out[at++] = 0; *written = at; return true;
}

/* Decode compression with a bounded pointer walk. Return the first byte after
   the encoded name, independently of where compression pointers lead. */
static size_t dns_name_decode(const uint8_t *message, size_t length, size_t offset,
                              char out[DNS_NAME_TEXT_BYTES+1]) {
    size_t at=offset, end=0, used=0, wire=1, steps=0;
    while (at<length && ++steps<=length) {
        uint8_t label=message[at++];
        if (!label) {
            out[used]=0;
            return end?end:at;
        }
        if ((label&0xc0)==0xc0) {
            if (at>=length) return 0;
            size_t target=((size_t)(label&0x3f)<<8)|message[at++];
            if (target>=length) return 0;
            if (!end) end=at;
            at=target;
            continue;
        }
        if ((label&0xc0) || label>length-at || wire+label+1>DNS_NAME_WIRE_BYTES) return 0;
        wire+=label+1;
        if (used) out[used++]='.';
        if (used+label>DNS_NAME_TEXT_BYTES) return 0;
        for (unsigned i=0;i<label;i++) {
            uint8_t c=message[at++];
            if (!c || c=='.') return 0;
            out[used++]=(char)(c>='A'&&c<='Z'?c+('a'-'A'):c);
        }
    }
    return 0;
}

/* Measure only the largest text form that DNS can represent. Each user byte
   is checked before reading; the second copy detects mutation/invalid memory.
   Output is exact-size owned heap storage and remains live across DNS waits. */
static int dns_user_name_copy(const char *source, char **result) {
    *result = NULL;
    uintptr_t base = (uintptr_t)source;
    size_t length;
    for (length = 0; length <= DNS_NAME_TEXT_BYTES; length++) {
        if (base > UINTPTR_MAX - length || !user_ok((const void *)(base + length), 1)) return -EFAULT;
        if (!*(const char *)(base + length)) break;
    }
    if (length > DNS_NAME_TEXT_BYTES) return -ENAMETOOLONG;
    char *name = kmalloc(length + 1);
    if (!name) return -ENOMEM;
    int copied = user_str(name, source, length + 1);
    if (copied < 0 || (size_t)copied != length) {
        kfree(name); return copied < 0 ? copied : -EINVAL;
    }
    *result = name; return 0;
}
