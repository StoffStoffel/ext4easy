#!/bin/bash
# Checks an image written by ext4stress with the Linux tools:
#   1. e2fsck must report a clean filesystem (no fixes needed)
#   2. every file/dir in the manifest must exist with the same SHA-256,
#      and nothing else may exist (besides lost+found)
#
#   wsl bash tests/verify_image.sh <image> <manifest>
set -uo pipefail
IMG="$1"
MANIFEST="$2"

echo "== e2fsck"
e2fsck -fn "$IMG" > /tmp/e2fsck.$$ 2>&1
# Every question e2fsck asks is a problem - except pure optimization hints
# ("extent tree could be shorter/narrower"), which Linux itself also leaves.
if grep '?' /tmp/e2fsck.$$ | grep -qv -E 'could be (shorter|narrower)'; then
    cat /tmp/e2fsck.$$
    echo "FAIL: e2fsck found problems"
    rm -f /tmp/e2fsck.$$
    exit 1
fi
grep -E 'could be (shorter|narrower)' /tmp/e2fsck.$$ | sed 's/^/note: /'
tail -1 /tmp/e2fsck.$$
rm -f /tmp/e2fsck.$$

echo "== content"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/root"
debugfs -R "rdump / $OUT/root" "$IMG" > /dev/null 2>&1
ROOT="$OUT/root"
# debugfs rdump of "/" creates a directory named after the root (empty name) or puts entries directly
[ -d "$ROOT/" ] || { echo "FAIL: rdump produced nothing"; exit 1; }

python3 - "$ROOT" "$MANIFEST" <<'PY'
import hashlib, os, sys
root, manifest = sys.argv[1], sys.argv[2]
# find where the tree landed (debugfs may create an extra directory level)
entries = [e for e in os.listdir(root)]
if len(entries) == 1 and os.path.isdir(os.path.join(root, entries[0])) and entries[0] in ('', '/'):
    root = os.path.join(root, entries[0])
want_files, want_dirs = {}, set()
with open(manifest, 'rb') as f:
    for line in f:
        parts = line.rstrip(b'\n').split(b'\t')
        if parts[0] == b'D':
            want_dirs.add(parts[1])
        else:
            want_files[parts[1]] = (parts[2].decode(), int(parts[3]))
have_files, have_dirs = {}, set()
rootb = root.encode()
for dirpath, dirnames, filenames in os.walk(rootb):
    rel = dirpath[len(rootb):] or b'/'
    for d in dirnames:
        p = (rel.rstrip(b'/') + b'/' + d)
        if p != b'/lost+found':
            have_dirs.add(p)
    dirnames[:] = [d for d in dirnames if (rel.rstrip(b'/') + b'/' + d) != b'/lost+found']
    for n in filenames:
        p = rel.rstrip(b'/') + b'/' + n
        with open(os.path.join(dirpath, n), 'rb') as g:
            data = g.read()
        have_files[p] = (hashlib.sha256(data).hexdigest(), len(data))
errors = 0
for p in sorted(want_dirs - have_dirs):
    print('missing dir', p); errors += 1
for p in sorted(have_dirs - want_dirs):
    print('extra dir', p); errors += 1
for p, v in sorted(want_files.items()):
    if p not in have_files:
        print('missing file', p); errors += 1
    elif have_files[p] != v:
        print('content differs', p, have_files[p][1], 'vs', v[1]); errors += 1
for p in sorted(set(have_files) - set(want_files)):
    print('extra file', p); errors += 1
print(f'{len(want_files)} files, {len(want_dirs)} dirs checked, {errors} problems')
sys.exit(1 if errors else 0)
PY
