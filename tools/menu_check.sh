#!/bin/sh
# Checks that every disc image in umd/ still reaches its menus. Each disc runs in the menu
# simulator (build/host/menutest) for 240 s without input; the pages that became ready for
# input (READY lines) and when the last one did are compared with tests/menu_baseline.txt.
#   tools/menu_check.sh            compare; exits 1 if a disc reaches fewer menu pages than before
#   tools/menu_check.sh --update   write the baseline from this build
# The baseline lists the local disc images, so it stays out of git (see .gitignore). Run it
# before a release: a disc that now stops on a black screen shows as a lost READY page.
cd "$(dirname "$0")/.." || exit 2
BASE=tests/menu_baseline.txt
SIM=build/host/menutest
OUT=$(mktemp)
[ -x "$SIM" ] || { echo "build the host tools first: make -C tools"; exit 2; }

ls umd | while IFS= read -r f; do
    case "$f" in *.iso|*.ISO|*.cso|*.CSO|*.zso|*.ZSO) ;; *) continue ;; esac
    log=$("$SIM" -d "umd/$f" 240 </dev/null 2>&1)
    case "$log" in "cannot read"*) printf '%s\tunreadable\t-\t0\n' "$f"; continue ;; esac
    pages=$(printf '%s\n' "$log" | awk '/ READY page /{print $NF}' | tr '\n' ',' | sed 's/,$//')
    last=$(printf '%s\n' "$log" | awk '/ READY page /{t=$1} END{print t ? int(t) : "-"}')
    errors=$(printf '%s\n' "$log" | grep -c 'vsmx:')
    printf '%s\t%s\t%s\t%s\n' "$f" "${pages:--}" "$last" "$errors"
done > "$OUT"

if [ "$1" = "--update" ]; then
    mkdir -p tests
    cp "$OUT" "$BASE"
    echo "baseline written: $(wc -l < "$BASE" | tr -d ' ') discs in $BASE"
    rm -f "$OUT"
    exit 0
fi
[ -f "$BASE" ] || { echo "no $BASE yet: run tools/menu_check.sh --update on a good build"; cat "$OUT"; rm -f "$OUT"; exit 2; }

awk -F'\t' '
    NR == FNR { pages[$1] = $2; last[$1] = $3; errors[$1] = $4; next }
    {
        if ($2 == "unreadable") { printf "unread   %s: the image cannot be read (moved or incomplete?)\n", $1; next }
        if (!($1 in pages)) { printf "new      %s: menu pages %s, ready at %s s\n", $1, $2, $3; next }
        n_old = pages[$1] == "-" ? 0 : split(pages[$1], a, ",");
        n_new = $2 == "-" ? 0 : split($2, b, ",");
        if (n_new < n_old) { printf "LOST     %s: menu pages %s, was %s\n", $1, $2, pages[$1]; bad = 1 }
        else if ($2 != pages[$1]) printf "changed  %s: menu pages %s, was %s\n", $1, $2, pages[$1];
        else if ($3 != "-" && last[$1] != "-" && $3 > last[$1] + 5) printf "slower   %s: ready at %s s, was %s s\n", $1, $3, last[$1];
        if ($4 > errors[$1]) printf "errors   %s: %d script errors, was %d\n", $1, $4, errors[$1];
        seen++
    }
    END { printf "%d discs checked\n", seen; exit bad }
' "$BASE" "$OUT"
status=$?
rm -f "$OUT"
exit $status
