#!/bin/bash
# Builds ext4 test images. Run inside WSL/Linux (no root needed):
#   wsl bash tests/make_test_images.sh build/testdata
# Creates:
#   plain.img  - ext4 directly on the image (current mkfs defaults)
#   gpt.img    - GPT disk with a small FAT-type partition and an ext4 partition
#   ext2.img   - ext2 without extents/journal
#   expected.txt - md5 sums of the regular files, for comparing
set -euo pipefail

OUT="${1:-build/testdata}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/root"
mkdir -p "$SRC"

# --- content -----------------------------------------------------------------
cd "$SRC"
echo "Hallo von ext4!" > hello.txt
mkdir -p docs/sub/deeper
echo "deep file" > docs/sub/deeper/file.txt
printf 'Grüße, Ärger, Übermut – ß € 日本語\n' > "docs/Umlaute äöü.txt"
echo "colon" > "with:colon.txt"
echo "question" > "what?.txt"
echo "star" > "star*.txt"
echo "trailing dot" > "trailingdot."
echo "trailing space" > "trailingspace "
echo "pipe" > "a|b<c>d.txt"
printf 'invalid utf8 name\n' > "$(printf 'latin1-\xe4\xf6\xfc.txt')"
echo "secret" > .hidden
mkdir .config && echo "x=1" > .config/settings
echo "Case1" > Readme.md
echo "Case2" > README.md

# Files of various sizes (block boundaries, multi-extent, large)
head -c 4096 /dev/urandom > exact4k.bin
head -c 4097 /dev/urandom > over4k.bin
head -c 1 /dev/urandom > onebyte.bin
: > empty.txt
head -c $((64 * 1024 * 1024 + 123)) /dev/urandom > big.bin
truncate -s 100M sparse.bin
printf 'start' | dd of=sparse.bin conv=notrunc status=none
printf 'end' | dd of=sparse.bin bs=1 seek=$((100 * 1024 * 1024 - 3)) conv=notrunc status=none

# Many files: forces an htree-indexed directory
mkdir many
for i in $(seq 1 3000); do echo "$i" > "many/file_$i.txt"; done

# Links
ln -s hello.txt link_rel
ln -s /docs/sub link_abs_dir
ln -s docs/sub/deeper/../deeper/file.txt link_dotdot
ln -s does-not-exist link_broken
ln -s loop_b loop_a
ln -s loop_a loop_b
ln -s "docs/sub/deeper/this/is/a/very/long/symlink/target/that/needs/more/than/sixty/bytes" link_long
ln -s docs/sub link_dir_rel
ln hello.txt hardlink.txt
mkfifo fifo

# --- expected checksums ---------------------------------------------------------
# Format: "<md5>  <path>\0". md5sum is fed via stdin because it would
# replace invalid UTF-8 bytes in printed names.
find . -type f -print0 | sort -z | while IFS= read -r -d '' f; do
    printf '%s  %s\0' "$(md5sum < "$f" | cut -d' ' -f1)" "$f"
done > "$OUT/expected.txt"

# --- images ------------------------------------------------------------------
rm -f "$OUT"/*.img
mkfs.ext4 -q -L testvol -d "$SRC" "$OUT/plain.img" 256M
mkfs.ext2 -q -L oldfs -d "$SRC" "$OUT/ext2.img" 256M

# GPT image: partition 1 = 16 MiB "Microsoft basic data", partition 2 = ext4
truncate -s 300M "$OUT/gpt.img"
sfdisk -q "$OUT/gpt.img" <<'EOF'
label: gpt
start=2048, size=32768, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
start=34816, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
EOF
mkfs.ext4 -q -L gptvol -d "$SRC" -E offset=$((34816 * 512)) "$OUT/gpt.img" 256M

for img in plain ext2; do
    e2fsck -fn "$OUT/$img.img" > /dev/null && echo "$img.img: fsck clean"
done
dumpe2fs -h "$OUT/plain.img" 2>/dev/null | grep -i "features"
ls -la "$OUT"
