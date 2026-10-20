#!/bin/sh
# Pull the few HP files this driver needs out of Apple's official "HP Printer Drivers 5.1.1"
# package (https://support.apple.com/kb/DL1888). Nothing gets installed, the package is only
# unpacked into a temp folder. Only uses stuff every Mac has (no Xcode, Python or Rosetta).
#
#   tools/fetch_hp_files.sh OUTDIR                       download (584 MB) and extract
#   HP_DMG=/path/to/HewlettPackardPrinterDrivers.dmg tools/fetch_hp_files.sh OUTDIR
#
# OUTDIR gets: sdtahs10.bin sdtahr10.bin (halftone screens), hp1100.ppd.gz, hp1100.icns
# Every file is checked against a known SHA-256, so a wrong or changed download just fails.
set -eu

URL="https://updates.cdn-apple.com/2021/macos/071-46903-20211101-0BD2764A-901C-41BA-9573-C17B8FDC4D90/HewlettPackardPrinterDrivers.dmg"
DMG_SHA256="523836b630431bc39b0170a17099099d6f821ef62ff29e6ec64ebb69b9954133"
BIN_SHA256="be9415ca182fec58a17bea00603ff5591612d80b391299e307676c870eea0ca9"
S10_SHA256="efa8a8e8c49da4f6383e20b8cd3187327f4be325dc888956aa631d61bed44cce"
R10_SHA256="bee7e551104522f1ccb70f9d1808103d203bc9d97c4f2d1d38e6ffda3d6e2e51"
PPD_SHA256="44e5ccb4e16a65fdc0f85fb50e391c3ea9cd3131abc1b6ebcf5acec206478d4c"
ICON_SHA256="4e57aeadfc9fd130ddadd2b9049c78f05a4ab35cd725f55db016cd5c1d4a5b50"
PRINTERS="Payload/Library/Printers"
BIN="$PRINTERS/hp/laserjet/p1100_1560_1600series/hpRaster.bundle/Contents/MacOS/rastertozjs"
PPD="$PRINTERS/PPDs/Contents/Resources/hp1100.ppd.gz"
ICON="$PRINTERS/hp/Icons/hp1100.icns"

OUT=${1:?usage: tools/fetch_hp_files.sh OUTDIR}
mkdir -p "$OUT"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/p1102hp.XXXXXX")
cleanup() {
    if [ -d "$WORK/mnt" ]; then
        hdiutil detach -quiet -force "$WORK/mnt" 2>/dev/null || diskutil unmount force "$WORK/mnt" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
check() { # expected-sha file
    got=$(sha "$2")
    if [ "$got" != "$1" ]; then
        echo "checksum mismatch for $(basename "$2")" >&2
        echo "  expected $1" >&2
        echo "  got      $got" >&2
        exit 1
    fi
}

DMG=${HP_DMG:-}
if [ -z "$DMG" ]; then
    DMG="$WORK/HewlettPackardPrinterDrivers.dmg"
    echo "downloading HP Printer Drivers 5.1.1 from Apple (584 MB)..."
    curl -fL --retry 3 --progress-bar -o "$DMG" "$URL"
fi
echo "checking the download..."
check "$DMG_SHA256" "$DMG"

echo "unpacking (nothing gets installed)..."
mkdir "$WORK/mnt"
# hdiutil's -mountpoint is deprecated since macOS 27 in favour of "diskutil image", so try
# both (hdiutil first, it is what older macOS versions have)
if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$WORK/mnt" "$DMG" >/dev/null 2>"$WORK/attach.err"; then
    if ! diskutil image attach --mountOptions nobrowse --readOnly --mountPoint "$WORK/mnt" "$DMG" >/dev/null 2>>"$WORK/attach.err"; then
        echo "could not open the disk image:" >&2
        cat "$WORK/attach.err" >&2
        exit 1
    fi
fi
PKG="$WORK/mnt/HewlettPackardPrinterDrivers.pkg"
[ -f "$PKG" ] || { echo "no HewlettPackardPrinterDrivers.pkg in the disk image" >&2; exit 1; }
pkgutil --expand-full "$PKG" "$WORK/x" >/dev/null
X="$WORK/x/HewlettPackardPrinterDrivers.pkg"
check "$BIN_SHA256" "$X/$BIN"

# where the symbols _sdtahs10 / _sdtahr10 live in that exact (checksummed) binary
dd if="$X/$BIN" of="$OUT/sdtahs10.bin" bs=1 skip=$((0x45940)) count=12288 2>/dev/null
dd if="$X/$BIN" of="$OUT/sdtahr10.bin" bs=1 skip=$((0x42940)) count=12288 2>/dev/null
check "$S10_SHA256" "$OUT/sdtahs10.bin"
check "$R10_SHA256" "$OUT/sdtahr10.bin"
cp "$X/$PPD" "$OUT/hp1100.ppd.gz"
cp "$X/$ICON" "$OUT/hp1100.icns"
check "$PPD_SHA256" "$OUT/hp1100.ppd.gz"
check "$ICON_SHA256" "$OUT/hp1100.icns"
echo "done, HP files are in $OUT"
