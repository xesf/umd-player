#!/bin/sh
# Runs the format checks on every ISO in a folder (default: umd/).
# Fails if a disc does not parse or a seek point does not start a GOP.
set -e
dir=${1:-umd}
here=$(dirname "$0")
make -s -C "$here"
bin="$here/../build/host/umdinfo"
fail=0
for iso in "$dir"/*.iso "$dir"/*.ISO; do
    [ -f "$iso" ] || continue
    if out=$("$bin" "$iso" -v 2>&1); then
        echo "ok    $(basename "$iso"): $(echo "$out" | tail -1)"
    else
        echo "FAIL  $(basename "$iso")"
        echo "$out" | grep -E 'bad|invalid|cannot' | head -5
        fail=1
    fi
done
exit $fail
