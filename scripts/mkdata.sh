#!/usr/bin/env bash
# Create the persistent data disk (build/data.img): MBR + one FAT32 partition labelled NOCTDATA,
# mounted by Nocturne at /data. An existing image is never touched, so its files survive rebuilds.
set -e
cd "$(dirname "$0")/.."
IMG=${1:-build/data.img}
SIZE_MB=${2:-512}
[ -f "$IMG" ] && exit 0
PART_START=2048
TOTAL=$((SIZE_MB * 2048))
PART_SECTORS=$((TOTAL - PART_START))
echo "  DATA $IMG"
mkdir -p "$(dirname "$IMG")"
python - "$IMG" $TOTAL $PART_START $PART_SECTORS <<'PY'
import sys, struct
path, total, start, count = sys.argv[1], *map(int, sys.argv[2:])
with open(path, "wb") as f:
    f.truncate(total * 512)
    mbr = bytearray(512)
    mbr[440:444] = struct.pack("<I", 0x44415441)  # disk signature "DATA"
    mbr[446:462] = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", start, count)
    mbr[510:512] = b"\x55\xaa"
    f.write(mbr)
PY
P="$IMG@@$((PART_START * 512))"
mformat -i "$P" -F -T $PART_SECTORS -H $PART_START -h 64 -s 32 -v NOCTDATA ::
mmd -i "$P" ::/bin ::/etc ::/src ::/projects
