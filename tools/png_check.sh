#!/bin/sh
# Decodes PNG files with tools/pngdec and with FFmpeg and compares the RGBA bytes.
#   png_check.sh <file.png>...       or with no arguments: ICON0/PIC1 of every ISO in umd/
#                                    and the subtitle PNGs of the first subtitle track found.
here=$(dirname "$0")
bin="$here/../build/host"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
make -s -C "$here"
if [ $# -eq 0 ]; then
    n=0
    for iso in umd/*.iso umd/*.ISO; do
        [ -f "$iso" ] || continue
        n=$((n + 1))
        for f in ICON0 PIC1; do
            "$bin/umdinfo" "$iso" -x UMD_VIDEO/$f.PNG > "$tmp/disc$n-$f.png" 2>/dev/null || rm -f "$tmp/disc$n-$f.png"
        done
        if [ ! -d "$tmp/subs" ] && "$bin/umdinfo" "$iso" | grep -q 'subtitle\[80'; then
            "$bin/umddemux" "$iso" "$tmp/subs" -s 0x80 -n 300 > /dev/null
        fi
    done
    set -- "$tmp"/disc*.png "$tmp"/subs/sub_80/*.png
fi
ok=0; bad=0
for png in "$@"; do
    [ -s "$png" ] || continue
    "$bin/pngdec" "$png" "$tmp/ours.rgba" > /dev/null || { echo "FAIL decode $png"; bad=$((bad + 1)); continue; }
    ffmpeg -nostdin -v error -y -i "$png" -f rawvideo -pix_fmt rgba "$tmp/ref.rgba"
    if cmp -s "$tmp/ours.rgba" "$tmp/ref.rgba"; then ok=$((ok + 1)); else echo "DIFF  $png"; bad=$((bad + 1)); fi
done
echo "$ok identical, $bad different or failed"
[ $bad -eq 0 ]
