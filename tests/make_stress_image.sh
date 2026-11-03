#!/bin/bash
# Creates an empty ext4 image for ext4stress (run in WSL/Linux):
#   bash tests/make_stress_image.sh <variant> <output.img>
# Variants:
#   default    current mkfs defaults (journal, metadata_csum_seed, orphan_file, ...)
#   block1k    1 KiB blocks (more block groups, deeper extent trees)
#   nocsum     without metadata checksums
#   nojournal  without journal
#   classic    without 64bit and flex_bg
#   ext3       ext3 layout (no extents) - writing must be refused
set -euo pipefail
VARIANT="$1"
OUT="$2"
rm -f "$OUT"
case "$VARIANT" in
    default)   mkfs.ext4 -q -F -L stress "$OUT" 256M ;;
    block1k)   mkfs.ext4 -q -F -L stress -b 1024 "$OUT" 128M ;;
    nocsum)    mkfs.ext4 -q -F -L stress -O ^metadata_csum,^metadata_csum_seed "$OUT" 256M ;;
    nojournal) mkfs.ext4 -q -F -L stress -O ^has_journal "$OUT" 256M ;;
    classic)   mkfs.ext4 -q -F -L stress -O ^64bit,^flex_bg "$OUT" 256M ;;
    ext3)      mkfs.ext3 -q -F -L stress "$OUT" 64M ;;
    *) echo "unknown variant $VARIANT"; exit 2 ;;
esac
