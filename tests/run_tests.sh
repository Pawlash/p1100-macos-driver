#!/bin/bash
# Run the native filter on every captured case and compare with HP's reference output.
#   tests/run_tests.sh [path-to-filter] [case-glob]
# Needs no Rosetta: the references in tests/ref were captured once with capture_ref.sh.
set -uo pipefail
cd "$(dirname "$0")"
FILTER=${1:-../build/rastertop1102}
PPD=ref/hp.ppd
OUT=$(mktemp -d "${TMPDIR:-/tmp}/p1102-test.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

# Where the native filter deliberately differs from HP, say what it must match instead.
#   Duplex=... without sides=...: HP ignores it, we do manual duplex -> same as the sides= case
#   1 page + duplex: printed one-sided
#   odd page count + duplex: HP's filter crashes; we must produce HP's simplex pages in duplex
#   order with a white page added: backs last-to-first, pause, fronts first-to-last
expected() {
    case $1 in
        dup3_DuplexNoTumble) echo ref/sides_long_dup3.zjs ;;
        dup5_DuplexNoTumble) echo ref/sides_long_dup5.zjs ;;
        dup3_DuplexTumble)   echo ref/sides_short_dup3.zjs ;;
        dup5_DuplexTumble)   echo ref/sides_short_dup5.zjs ;;
        dup5_letter)         echo ref/sides_long_letter.zjs ;;
        sides_long_1page)    echo ref/size_A4.zjs ;;
        hdr_compression_0)   echo ref/hdr_compression_1.zjs ;;  # HP: broken 1-bit mode; we: 2-bit
        sides_long_odd3)
            python3 -I ../tools/zjscompose.py "$OUT/$1.expected" \
                ref/white_a4.zjs:1 ref/plain_odd3.zjs:2 PAUSE ref/plain_odd3.zjs:1 ref/plain_odd3.zjs:3
            echo "$OUT/$1.expected" ;;
        sides_short_odd5)
            python3 -I ../tools/zjscompose.py "$OUT/$1.expected" \
                ref/white_a4.zjs:1 ref/plain_odd5.zjs:4 ref/plain_odd5.zjs:2 PAUSE \
                ref/plain_odd5.zjs:1 ref/plain_odd5.zjs:3 ref/plain_odd5.zjs:5
            echo "$OUT/$1.expected" ;;
        *) echo "ref/$1.zjs" ;;
    esac
}

pass=0 fail=0
for c in cases/${2:-*}.opts; do
    n=$(basename "$c" .opts)
    r=$(cat "cases/$n.ras.link")
    [ -f "ras/$r.ras.gz" ] || { echo "SKIP $n (raster missing, run capture_ref.sh)"; continue; }
    title=hello
    [ "$n" = title_utf8 ] && title="Zażółć gęślą jaźń – test.pdf"
    copies=$(grep -o 'copies=[0-9]*' "$c" | cut -d= -f2 || true)
    # rasters are stored gzipped (they are mostly white); the filter reads stdin like in CUPS
    gunzip -c "ras/$r.ras.gz" | PPD=$PPD "$FILTER" 1 pawel "$title" "${copies:-1}" "$(cat "$c")" \
        > "$OUT/$n.zjs" 2> "$OUT/$n.log"
    rc=$?
    ref=$(expected "$n")
    if [ $rc -eq 0 ] && cmp -s "$OUT/$n.zjs" "$ref"; then
        pass=$((pass + 1)); echo "ok   $n"
    else
        fail=$((fail + 1)); echo "FAIL $n (rc=$rc) $(cmp "$OUT/$n.zjs" "$ref" 2>&1 | head -1)"
        cp "$OUT/$n.zjs" "$OUT/../p1102-fail-$n.zjs" 2>/dev/null
    fi
    rm -f "$OUT/$n.zjs"
done
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ]
