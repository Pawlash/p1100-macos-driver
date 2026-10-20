#!/bin/bash
# Capture reference output of HP's original (x86_64, Rosetta) rastertozjs filter.
# Each case: a CUPS raster input (tests/ras/NAME.ras) + job options (tests/cases/NAME.opts)
# -> tests/ref/NAME.zjs. Run this while Rosetta still exists; the results are kept as fixtures.
set -euo pipefail
cd "$(dirname "$0")"
HPF=/Library/Printers/hp/laserjet/p1100_1560_1600series/hpRaster.bundle/Contents/MacOS/rastertozjs
PPD=ref/hp.ppd
RASTOOL=../build/rastool
mkdir -p ras cases ref in

# --- source documents -------------------------------------------------------
if [ ! -f in/text3.pdf ]; then
    {
        for p in 1 2 3; do
            echo "Strona $p / Page $p"
            for i in $(seq 1 50); do echo "Linia $i: Zażółć gęślą jaźń. The quick brown fox jumps over the lazy dog 0123456789"; done
            [ $p -lt 3 ] && printf '\f'
        done
    } > in/text3.txt
    cupsfilter -m application/pdf in/text3.txt > in/text3.pdf 2>/dev/null
fi
if [ ! -f in/text5.pdf ]; then
    {
        for p in 1 2 3 4 5; do
            echo "Page $p of 5"
            for i in $(seq 1 $((p * 8))); do echo "line $i"; done
            [ $p -lt 5 ] && printf '\f'
        done
    } > in/text5.txt
    cupsfilter -m application/pdf in/text5.txt > in/text5.pdf 2>/dev/null
fi

# raster from PDF through Apple's own pipeline (same as real printing)
mkras() { # name pdf options...
    local name=$1 pdf=$2; shift 2
    local o=()
    for x in "$@"; do o+=(-o "$x"); done
    cupsfilter -p "$PPD" -m application/vnd.cups-raster "${o[@]}" "$pdf" > "ras/$name.ras" 2>/dev/null
}

addcase() { # name raster options-string
    echo "$2" > "cases/$1.ras.link"
    echo "$3" > "cases/$1.opts"
}

# --- page sizes ---------------------------------------------------------------
for ps in Letter Legal A4 Executive 8.5x13Custom A5 A6 B5JIS 16K195x270mm 16K184x260mm 16K197x273mm \
          Postcard DoublePostcard Env10 Monarch EnvISOB5 EnvC5 EnvDL Custom.400x700 Custom.216x360; do
    n="size_$ps"
    mkras "$n" in/hello.pdf "PageSize=$ps"
    addcase "$n" "$n" "PageSize=$ps"
done

# --- media types ----------------------------------------------------------------
for mt in Paper Light60-74g Mid-Weight96-110g Heavy111-130g ExtraHeavy131-175g TransparencyPaper \
          LabelsPaper LetterheadPaper EnvelopePaper PreprintedPaper PrepunchedPaper ColorPaper \
          BondPaper RecycledPaper RoughPaper VellumPaper; do
    n="media_$mt"
    mkras "$n" in/hello.pdf "PageSize=A4" "MediaType=$mt"
    addcase "$n" "$n" "PageSize=A4 MediaType=$mt"
done

# --- resolution / quality / PJL options ----------------------------------------
mkras res600 in/text3.pdf "PageSize=A4" "Resolutions=600x600dpi"
addcase res1200_text3 res600 "PageSize=A4 Resolutions=600x600dpi"
mkras res400 in/text3.pdf "PageSize=A4" "Resolutions=600x400dpi"
addcase res600_text3 res400 "PageSize=A4 Resolutions=600x400dpi"
addcase econo_on res400 "PageSize=A4 EconoMode=True"
addcase econo_on_1200 res600 "PageSize=A4 Resolutions=600x600dpi EconoMode=True"
for d in 1 2 4 5; do addcase "density_$d" res400 "PageSize=A4 pjlDensity=$d"; done
addcase jam_on res400 "PageSize=A4 pjlJamRecovery=True"
addcase copies3 res400 "PageSize=A4 copies=3"
addcase copies2_collate res400 "PageSize=A4 copies=2 Collate=True"
addcase title_utf8 res400 "PageSize=A4"

# --- duplex (manual) --------------------------------------------------------------
for dx in DuplexNoTumble DuplexTumble; do
    mkras "dup3_$dx" in/text3.pdf "PageSize=A4" "Duplex=$dx"
    addcase "dup3_$dx" "dup3_$dx" "PageSize=A4 Duplex=$dx"
    mkras "dup5_$dx" in/text5.pdf "PageSize=A4" "Duplex=$dx"
    addcase "dup5_$dx" "dup5_$dx" "PageSize=A4 Duplex=$dx"
done
mkras dup5_letter in/text5.pdf "PageSize=Letter" "Duplex=DuplexNoTumble"
addcase dup5_letter dup5_letter "PageSize=Letter Duplex=DuplexNoTumble"
# macOS passes the IPP "sides" attribute; that is what the HP filter actually looks at
for n in dup3_DuplexNoTumble dup5_DuplexNoTumble; do
    addcase "sides_long_${n%%_*}" "$n" "PageSize=A4 Duplex=DuplexNoTumble sides=two-sided-long-edge"
done
for n in dup3_DuplexTumble dup5_DuplexTumble; do
    addcase "sides_short_${n%%_*}" "$n" "PageSize=A4 Duplex=DuplexTumble sides=two-sided-short-edge"
done
addcase sides_long_letter dup5_letter "PageSize=Letter Duplex=DuplexNoTumble sides=two-sided-long-edge"
addcase sides_long_1page size_A4 "PageSize=A4 Duplex=DuplexNoTumble sides=two-sided-long-edge"
addcase sides_long_mixed4 pat_mixed4 "PageSize=A4 sides=two-sided-long-edge"
addcase sides_onesided res400 "PageSize=A4 sides=one-sided"
$RASTOOL pattern ras/size_A4.ras ras/pat_mixed3.ras mixed 3
$RASTOOL pattern ras/size_A4.ras ras/pat_mixed5.ras mixed 5
addcase sides_long_odd3 pat_mixed3 "PageSize=A4 sides=two-sided-long-edge"
addcase sides_short_odd5 pat_mixed5 "PageSize=A4 sides=two-sided-short-edge"
addcase plain_odd3 pat_mixed3 "PageSize=A4"
addcase plain_odd5 pat_mixed5 "PageSize=A4"
$RASTOOL pattern ras/size_A4.ras ras/white_a4.ras white 1
addcase white_a4 white_a4 "PageSize=A4"
# header-field probes (mapping of tray, media, copies, resolution label)
for kv in copies=3 mediaposition=1 mediaposition=0 compression=0 compression=1 mediatype=999; do
    n="hdr_$(echo $kv | tr = _)"
    $RASTOOL set ras/size_A4.ras "ras/$n.ras" "$kv"
    addcase "$n" "$n" "PageSize=A4"
done

# --- synthetic content: exercises screening and the JBIG coder hard ---------------
for k in black ramp levels noise mixed; do
    $RASTOOL pattern ras/res400.ras "ras/pat_$k.ras" "$k" 1
    addcase "pat_$k" "pat_$k" "PageSize=A4"
    $RASTOOL pattern ras/res600.ras "ras/pat1200_$k.ras" "$k" 1
    addcase "pat1200_$k" "pat1200_$k" "PageSize=A4 Resolutions=600x600dpi"
done
$RASTOOL pattern ras/res400.ras ras/pat_mixed4.ras mixed 4
addcase pat_mixed4 pat_mixed4 "PageSize=A4"
$RASTOOL pattern ras/size_Letter.ras ras/pat_letter_mixed.ras mixed 1
addcase pat_letter_mixed pat_letter_mixed "PageSize=Letter"
$RASTOOL pattern ras/size_Env10.ras ras/pat_env10_noise.ras noise 1
addcase pat_env10_noise pat_env10_noise "PageSize=Env10"

# --- run the HP filter on every case ----------------------------------------------
for c in cases/${ONLY:-*}.opts; do
    n=$(basename "$c" .opts)
    r=$(cat "cases/$n.ras.link")
    title=hello
    [ "$n" = title_utf8 ] && title="Zażółć gęślą jaźń – test.pdf"
    copies=$(grep -o 'copies=[0-9]*' "$c" | cut -d= -f2 || true)
    DEVICE_URI=file:///dev/null PPD=$PPD "$HPF" 1 pawel "$title" "${copies:-1}" "$(cat "$c")" "ras/$r.ras" \
        > "ref/$n.zjs" 2> "ref/$n.log" || echo "FAILED: $n (rc=$?)"
done
ls ref/*.zjs | wc -l
# keep the inputs small: run_tests.sh reads ras/*.ras.gz
for f in ras/*.ras; do [ -f "$f" ] && gzip -6 -f "$f"; done
