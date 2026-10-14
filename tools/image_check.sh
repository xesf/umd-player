#!/bin/sh
# Checks the CSO and ZSO readers without a disc: builds a 24 MiB test image (zero runs,
# repeated text, and random data, so every block type appears), compresses it with
# tools/mkcso.py, and reads both copies back with tools/imgcmp.
#   image_check.sh            (needs python3, and the lz4 command for the ZSO part)
set -e
here=$(dirname "$0")
bin="$here/../build/host"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
make -s -C "$here"
python3 -I - "$tmp/test.iso" <<'PY'
import os, sys
with open(sys.argv[1], "wb") as f:
    for i in range(12):
        f.write(b"\0" * (512 * 1024))
        f.write(((b"UMD player test block %d " % i) * (512 * 1024 // 16))[:512 * 1024])
        f.write(os.urandom(512 * 1024))
        f.write(bytes(range(256)) * (2048 * 2 // 256))
    size = f.tell()
    f.write(b"\0" * (-size % 2048))
PY
fail=0
for ext in cso zso; do
    if [ $ext = zso ] && ! command -v lz4 > /dev/null; then
        echo "skip  zso: no lz4 command"
        continue
    fi
    python3 -I "$here/mkcso.py" "$tmp/test.iso" "$tmp/test.$ext"
    if out=$("$bin/imgcmp" "$tmp/test.iso" "$tmp/test.$ext"); then
        echo "ok    $ext: $out"
    else
        echo "FAIL  $ext: $out"
        fail=1
    fi
done
exit $fail
