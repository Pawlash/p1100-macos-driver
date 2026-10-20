#!/bin/sh
# Switch queues back to HP's original driver (needs the HP driver files and Rosetta).
#   sudo /Library/Printers/P1102Native/use-hp-driver.sh
BACKUP=/Library/Printers/P1102Native/backup
if [ "$(id -u)" != 0 ]; then exec sudo "$0" "$@"; fi
HPF=/Library/Printers/hp/laserjet/p1100_1560_1600series/hpRaster.bundle/Contents/MacOS/rastertozjs
if [ ! -x "$HPF" ]; then
    echo "HP's driver is not installed any more ($HPF missing) - nothing to switch back to."
    exit 1
fi
found=0
for f in "$BACKUP"/*.ppd; do
    [ -f "$f" ] || continue
    q=$(basename "$f" .ppd)
    echo "Switching queue $q back to HP's driver"
    /usr/sbin/lpadmin -p "$q" -P "$f" && found=1
done
[ $found = 1 ] || echo "No saved HP queue settings found in $BACKUP."
