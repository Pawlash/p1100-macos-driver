#!/bin/sh
# (Re)attach the native driver to every P1100-series queue, e.g. after use-hp-driver.sh.
#   sudo /Library/Printers/P1102Native/use-native-driver.sh
if [ "$(id -u)" != 0 ]; then exec sudo "$0" "$@"; fi
PPDGZ="/Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1100 Native.ppd.gz"
TMPPPD=$(mktemp /tmp/p1102native.XXXXXX) || exit 1
gunzip -c "$PPDGZ" > "$TMPPPD"
found=0
for f in /etc/cups/ppd/*.ppd; do
    [ -f "$f" ] || continue
    grep -qE '^\*ModelName: "HP LaserJet Professional P1100( Native)?"' "$f" || continue
    q=$(basename "$f" .ppd)
    echo "Switching queue $q to the native driver"
    /usr/sbin/lpadmin -p "$q" -P "$TMPPPD" && /usr/sbin/lpadmin -p "$q" -o PageSize=A4 -o PageRegion=A4 -o InputSlot=Auto -o printer-error-policy=retry-job
    found=1
done
rm -f "$TMPPPD"
[ $found = 1 ] || echo "No P1100-series queue found. Add the printer in System Settings > Printers & Scanners; macOS will pick 'HP LaserJet Professional P1100, native driver'."
