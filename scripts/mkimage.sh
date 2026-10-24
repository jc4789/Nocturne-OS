#!/usr/bin/env bash
# Build the boot disk: one GPT disk with an EFI system partition (Limine and the kernel) and the
# Nocturne partition, drive C: (scripts/layout.py says what goes on it). From the raw disk (QEMU)
# come a fixed VHD (Hyper-V Gen1) and a dynamic VHDX (Hyper-V Gen2). The ISO (BIOS and UEFI) is a
# live system: Limine brings its C:, a bare volume, into memory with the kernel.
#   --raw-only    the raw disk alone, for a QEMU test boot
#   --run         the raw disk, then `make run`'s own disk, build/run.img: a copy of it the first
#                 time, updated (--update) after that
#   --update DISK only this: the system of the raw disk DISK (its boot part, C:\Nocturne and the
#                 desktop apps' folders) is replaced by the built one; the user's files and the
#                 apps the user put in C:\Programs stay (hyperv.ps1 uses it)
#   --hyperv RAW DISK
#                 only this: Hyper-V's disk DISK (.vhd or .vhdx) from the raw disk RAW
set -e
cd "$(dirname "$0")/.."
B=build
LIMINE=tools/limine
STORE=$B/host/store.exe
IMG=$B/nocturne.img
ISO=$B/nocturne.iso
FIRMWARE=$B/nvidia-firmware.json
ESP_MB=64
FREE_MB=128     # room on C: before it grows: the kernel takes C: to the end of the disk the image
                # is written to on its first start, so the image itself carries little more than
                # what is put on it
ISO_FREE_MB=256 # the room on the ISO's C:, which takes its whole size of memory and never grows
RUN_MB=16384    # build/run.img, sparse: C: grows into it on its first start
VHD_MB=4096     # Hyper-V Gen 1's fixed VHD (a sparse file), at least
VHDX_MB=65536   # Hyper-V Gen 2's dynamic VHDX, at least: its file holds only what is written
MIB=$((1024 * 1024))

# The boot part of disk $1: its EFI system partition (FAT: Limine and the kernel) and Limine's BIOS
# stage in its BIOS boot partition, the first.
boot_install() {
    local offset sectors
    read -r offset sectors <<< "$($STORE esp "$1")"
    local esp="$1@@$offset"
    mformat -i "$esp" -F -T "$sectors" -H $((offset / 512)) -h 64 -s 32 -v NOCTURNE ::
    mmd -i "$esp" ::/boot ::/boot/limine ::/EFI ::/EFI/BOOT
    mcopy -i "$esp" $B/kernel.elf ::/boot/kernel.elf
    mcopy -i "$esp" boot/limine.conf $LIMINE/limine-bios.sys ::/boot/limine/
    mcopy -i "$esp" $LIMINE/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
    local out
    out=$($B/host/limine.exe bios-install "$1" 1 2>&1) || { echo "$out" >&2; return 1; }
}

# Hyper-V's disk $2 from the raw disk $1: a fixed VHD (.vhd, Generation 1) or a dynamic VHDX
# (.vhdx, Generation 2), of the raw disk's size or larger (VHD_MB, VHDX_MB): C: grows to its end
# on the next start
hyperv_disk() {
    local fmt opts min size
    case "$2" in
    *.vhd) fmt=vpc opts=subformat=fixed,force_size=on min=$VHD_MB ;;
    *.vhdx) fmt=vhdx opts=subformat=dynamic min=$VHDX_MB ;;
    *) echo "mkimage: $2 is neither a .vhd nor a .vhdx" >&2; return 1 ;;
    esac
    size=$(($(stat -c %s "$1") / MIB))
    [ "$size" -ge "$min" ] || size=$min
    rm -f "$2"
    qemu-img create -q -f $fmt -o $opts "$2" "${size}M"
    qemu-img convert -n --target-is-zero -f raw -O $fmt "$1" "$2"
}

# The ISO: Limine for BIOS and UEFI, the kernel, and C: as boot/c.img, a bare volume of the size
# $1 MiB, which every entry of boot/limine.conf brings along as the module "C:"
iso_make() {
    local root=$B/iso_root
    rm -rf $root $ISO
    mkdir -p $root/boot/limine $root/EFI/BOOT
    cp $B/kernel.elf $root/boot/
    sed 's|^\( *\)path: boot():/boot/kernel.elf$|&\n\1module_path: boot():/boot/c.img\n\1module_string: C:|' \
        boot/limine.conf > $root/boot/limine/limine.conf
    cp $LIMINE/limine-bios.sys $LIMINE/limine-bios-cd.bin $LIMINE/limine-uefi-cd.bin $root/boot/limine/
    cp $LIMINE/BOOTX64.EFI $root/EFI/BOOT/
    truncate -s "$1M" $root/boot/c.img
    $STORE format $root/boot/c.img Nocturne
    echo "  C:   $($STORE fill $root/boot/c.img $B/c.list)"
    xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
        -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label $root -o $ISO 2>/dev/null
    rm -rf $root
    local out
    out=$($B/host/limine.exe bios-install $ISO 2>&1) || { echo "$out" >&2; return 1; }
}

# The built system in place of the one on disk $1.
update() {
    python scripts/layout.py --firmware-manifest $FIRMWARE --system $B/system.list >/dev/null
    boot_install "$1"
    echo "  C:   $($STORE fill "$1" $B/system.list)"
}

if [ "$1" = --update ]; then
    [ -f "$2" ] || { echo "mkimage: there is no disk $2" >&2; exit 1; }
    echo "  UPD  $2 (its system replaced, the user's files kept)"
    update "$2"
    exit 0
fi
if [ "$1" = --hyperv ]; then
    echo "  DISK $3"
    hyperv_disk "$2" "$3"
    exit 0
fi

PAYLOAD=$(python scripts/layout.py --firmware-manifest $FIRMWARE $B/c.list)
# C: holds what is put on it, its catalog and a little room; the disk is whole 64 MiB
SIZE_MB=$(((ESP_MB + 64 + PAYLOAD / MIB * 105 / 100 + FREE_MB + 63) / 64 * 64))

echo "  IMG  $IMG ($SIZE_MB MiB)"
rm -f $IMG
$STORE disk $IMG $SIZE_MB $ESP_MB
boot_install $IMG
$STORE format $IMG Nocturne
echo "  C:   $($STORE fill $IMG $B/c.list)"

case "$1" in
--raw-only)
    exit 0 ;;
--run)
    RUN=$B/run.img
    if [ ! -f $RUN ]; then
        echo "  RUN  $RUN (new, $RUN_MB MiB)"
        qemu-img convert -f raw -O raw $IMG $RUN
        qemu-img resize -q -f raw $RUN ${RUN_MB}M
    else
        echo "  RUN  $RUN (its system replaced, the user's files kept)"
        update $RUN
    fi
    exit 0 ;;
esac

echo "  VHD  $B/nocturne.vhd"
hyperv_disk $IMG $B/nocturne.vhd
echo "  VHDX $B/nocturne.vhdx"
hyperv_disk $IMG $B/nocturne.vhdx
ISO_C_MB=$(((64 + PAYLOAD / MIB * 105 / 100 + ISO_FREE_MB + 63) / 64 * 64))
echo "  ISO  $ISO (C: $ISO_C_MB MiB, in memory)"
iso_make $ISO_C_MB
echo "  done."
