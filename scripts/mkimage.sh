#!/usr/bin/env bash
# Build bootable images: raw disk (QEMU), fixed VHD (Hyper-V Gen1), VHDX and a hybrid ISO.
set -e
cd "$(dirname "$0")/.."
B=build
LIMINE=tools/limine
IMG=$B/nocturne.img
# Leave room for FAT metadata and optional diskfiles as the RAM-root grows.
# The old fixed 128 MiB image cannot hold the bundled multilingual fonts.
SIZE_MB=$(python - <<'EOF'
from pathlib import Path
payload = [Path(p) for p in (
    "build/kernel.elf", "build/initrd.tar", "boot/limine.conf",
    "tools/limine/limine-bios.sys", "tools/limine/BOOTX64.EFI")]
payload += [p for p in Path("diskfiles").rglob("*") if p.is_file()]
mib = 1024 * 1024
needed = sum(p.stat().st_size for p in payload) + 32 * mib
print(max(128, ((needed + 64 * mib - 1) // (64 * mib)) * 64))
EOF
)
PART_START=2048
TOTAL=$((SIZE_MB * 2048))
PART_SECTORS=$((TOTAL - PART_START))

echo "  IMG  $IMG ($SIZE_MB MiB)"
rm -f $IMG
python - "$IMG" $TOTAL $PART_START $PART_SECTORS <<'EOF'
import sys, struct
path, total, start, count = sys.argv[1], *map(int, sys.argv[2:])
with open(path, "wb") as f:
    f.truncate(total * 512)
    mbr = bytearray(512)
    mbr[440:444] = struct.pack("<I", 0x4E4F4354)  # disk signature "NOCT"
    entry = struct.pack("<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", start, count)
    mbr[446:462] = entry
    mbr[510] = 0x55
    mbr[511] = 0xAA
    f.write(mbr)
EOF

P="$IMG@@$((PART_START * 512))"
mformat -i "$P" -F -T $PART_SECTORS -H $PART_START -h 64 -s 32 -v NOCTURNE ::
mmd -i "$P" ::/boot ::/boot/limine ::/EFI ::/EFI/BOOT ::/home
mcopy -i "$P" $B/kernel.elf ::/boot/kernel.elf
mcopy -i "$P" $B/initrd.tar ::/boot/initrd.tar
mcopy -i "$P" boot/limine.conf ::/boot/limine/limine.conf
mcopy -i "$P" $LIMINE/limine-bios.sys ::/boot/limine/limine-bios.sys
mcopy -i "$P" $LIMINE/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
if [ -d diskfiles ]; then
    for f in diskfiles/*; do mcopy -i "$P" -s "$f" ::/home/; done
fi
$LIMINE/limine.exe bios-install $IMG >/dev/null

echo "  VHD  $B/nocturne.vhd"
rm -f $B/nocturne.vhd $B/nocturne.vhdx
qemu-img convert -f raw -O vpc -o subformat=fixed,force_size=on $IMG $B/nocturne.vhd
qemu-img convert -f raw -O vhdx -o subformat=dynamic $IMG $B/nocturne.vhdx

echo "  ISO  $B/nocturne.iso"
rm -rf $B/iso_root
mkdir -p $B/iso_root/boot/limine $B/iso_root/EFI/BOOT
cp $B/kernel.elf $B/initrd.tar $B/iso_root/boot/
cp boot/limine.conf $LIMINE/limine-bios.sys $LIMINE/limine-bios-cd.bin $LIMINE/limine-uefi-cd.bin $B/iso_root/boot/limine/
cp $LIMINE/BOOTX64.EFI $B/iso_root/EFI/BOOT/
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
    -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label $B/iso_root -o $B/nocturne.iso 2>/dev/null
$LIMINE/limine.exe bios-install $B/nocturne.iso >/dev/null
echo "  done."
