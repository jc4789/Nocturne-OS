/* ATA PIO disk driver (dev/ata.c). Sectors are 512 bytes. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

struct ata_disk {
    int index;
    uint16_t io, ctl;
    bool slave, lba48;
    uint64_t sectors;
    char model[41];
};

void ata_init(void);
int ata_count(void);
struct ata_disk *ata_get(int i);
int ata_read(struct ata_disk *d, uint64_t lba, uint32_t count, void *buf);
int ata_write(struct ata_disk *d, uint64_t lba, uint32_t count, const void *buf);
