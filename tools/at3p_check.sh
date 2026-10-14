#!/bin/sh
# Decodes the first N seconds (default 60) of every audio track on every ISO in a folder
# with tools/at3pdec and with FFmpeg, and compares the PCM. FFmpeg is the reference only.
set -e
dir=${1:-umd}
seconds=${2:-60}
here=$(dirname "$0")
bin="$here/../build/host"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
make -s -C "$here"
fail=0
for iso in "$dir"/*.iso "$dir"/*.ISO; do
    [ -f "$iso" ] || continue
    tracks=$("$bin/umdinfo" "$iso" | grep -o 'audio\[[0-9a-f]*' | sort -u | sed 's/audio\[//')
    for t in $tracks; do
        "$bin/umddemux" "$iso" "$tmp/d" -a "0x$t" -n "$seconds" > /dev/null
        "$bin/at3pdec" "$tmp/d/audio_$t.oma" "$tmp/ours.f32" -f > /dev/null
        ffmpeg -v error -y -i "$tmp/d/audio_$t.oma" -f f32le "$tmp/ref.f32"
        if r=$(python3 -I "$here/pcmcmp.py" "$tmp/ours.f32" "$tmp/ref.f32"); then
            echo "ok    $(basename "$iso") audio $t: $r"
        else
            echo "FAIL  $(basename "$iso") audio $t: $r"
            fail=1
        fi
        rm -rf "$tmp/d"
    done
done
exit $fail
