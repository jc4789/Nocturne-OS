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
