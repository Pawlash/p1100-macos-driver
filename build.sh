#!/bin/bash
# Build the native P1100 series driver: filter, PPD, installer package and DMG.
#   ./build.sh                build the installer package into dist/
#   ./build.sh --test         also run the byte-for-byte comparison against HP's references
#   ./build.sh --dmg          also wrap the package in a disk image (for publishing)
set -euo pipefail
cd "$(dirname "$0")"
VERSION=1.3
ARGS=("$@")
ID=local.p1102native.driver
B=build
mkdir -p "$B" dist

HP=hp   # HP's files (screens, PPD, icon); local only, never committed
if [ ! -f "$HP/sdtahs10.bin" ] || [ ! -f "$HP/sdtahr10.bin" ] || [ ! -f "$HP/hp1100.ppd.gz" ] || [ ! -f "$HP/hp1100.icns" ]; then
    echo "== HP files (one-time download from Apple)"
    tools/fetch_hp_files.sh "$HP"
fi

echo "== tables"
python3 -I tools/gen_tables.py "$HP" "$B/screens.h"

echo "== filter (universal: arm64 + x86_64)"
clang -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
    -I"$B" -o "$B/rastertop1102" src/rastertop1102.c -lcups
codesign -s - -f "$B/rastertop1102"
lipo -archs "$B/rastertop1102"

clang -O2 -Wall -Wno-deprecated-declarations -o "$B/rastool" tools/rastool.c -lcups
want() { for a in "${ARGS[@]:-}"; do [ "$a" = "$1" ] && return 0; done; return 1; }
if want --test && [ -d tests/ref ]; then
    echo "== tests"
    tests/run_tests.sh "$PWD/$B/rastertop1102"
fi

echo "== PPD"
python3 -I ppd/make_ppd.py "$HP/hp1100.ppd.gz" "$B/P1100-native.ppd"
cupstestppd -W none "$B/P1100-native.ppd" 2>&1 | grep -E 'FAIL' | grep -vE 'Missing (cupsFilter|APPrinterIconPath) file|^.*: FAIL$' && exit 1 || true

echo "== package"
ROOT="$B/root"
rm -rf "$ROOT"
install -d -m 755 "$ROOT/Library/Printers/P1102Native" "$ROOT/Library/Printers/PPDs/Contents/Resources"
install -m 755 "$B/rastertop1102" "$ROOT/Library/Printers/P1102Native/"
install -m 644 "$HP/hp1100.icns" "$ROOT/Library/Printers/P1102Native/printer.icns"
install -m 644 pkg/README.txt "$ROOT/Library/Printers/P1102Native/"
for s in use-hp-driver.sh use-native-driver.sh remove-hp-driver.sh uninstall.sh; do
    install -m 755 "pkg/support/$s" "$ROOT/Library/Printers/P1102Native/"
done
gzip -9 -n -c "$B/P1100-native.ppd" > "$ROOT/Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1100 Native.ppd.gz"
chmod 644 "$ROOT/Library/Printers/PPDs/Contents/Resources/"*.gz
chmod 755 pkg/scripts/postinstall
xattr -cr "$ROOT"

pkgbuild --root "$ROOT" --scripts pkg/scripts --identifier "$ID" --version "$VERSION" \
    --install-location / --ownership recommended "$B/component-raw.pkg" 2>/dev/null >/dev/null
# pkgbuild copies extended attributes as AppleDouble "._" files and uses its own idea of
# directory permissions; rebuild Payload + Bom so the package holds exactly our files with
# the owners/modes macOS already has (installing must not touch /Library/Printers itself).
X="$B/component-x"
rm -rf "$X" "$B/component.pkg"
pkgutil --expand "$B/component-raw.pkg" "$X"
python3 -I tools/mkpayload.py "$ROOT" "$B/payload.mtree" "$B/payload.bomlist" >/dev/null
COPYFILE_DISABLE=1 tar --format odc --no-xattrs --no-mac-metadata -cf - "@$B/payload.mtree" \
    | gzip -9 > "$X/Payload"
rm -f "$X/Bom"
mkbom -i "$B/payload.bomlist" "$X/Bom"
nfiles=$(wc -l < "$B/payload.bomlist" | tr -d ' ')
kbytes=$(( ($(gunzip -c "$X/Payload" | wc -c) + 1023) / 1024 ))
sed -i '' -E "s/numberOfFiles=\"[0-9]+\"/numberOfFiles=\"$nfiles\"/; s/installKBytes=\"[0-9]+\"/installKBytes=\"$kbytes\"/" "$X/PackageInfo"
pkgutil --flatten "$X" "$B/component.pkg"
if lsbom -s "$X/Bom" | grep -q '/\._'; then echo "AppleDouble files in package" >&2; exit 1; fi
cat > "$B/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>P1100 Series Native Driver $VERSION</title>
    <readme file="README.txt" mime-type="text/plain"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default" title="P1100 Series Native Driver"><pkg-ref id="$ID"/></choice>
    <pkg-ref id="$ID" version="$VERSION" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
EOF
mkdir -p "$B/resources"
cp pkg/README.txt "$B/resources/"
productbuild --distribution "$B/distribution.xml" --resources "$B/resources" \
    --package-path "$B" "dist/P1100 Series Native Driver.pkg" >/dev/null

rm -f dist/*.dmg
if ! want --dmg; then ls -la dist/; exit 0; fi
echo "== dmg"
STAGE="$B/dmg"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "dist/P1100 Series Native Driver.pkg" pkg/README.txt "$STAGE/"
rm -f "dist/P1100-Series-Native-Driver-$VERSION.dmg"
hdiutil create -quiet -volname "P1100 Series Native Driver $VERSION" -srcfolder "$STAGE" -fs HFS+ \
    -format UDZO "dist/P1100-Series-Native-Driver-$VERSION.dmg"
ls -la dist/
