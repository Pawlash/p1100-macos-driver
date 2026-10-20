#!/bin/sh
# Remove the native driver. Queues are switched back to HP's driver when it is still
# installed; otherwise they are left without a working driver (re-add the printer later).
#   sudo /Library/Printers/P1102Native/uninstall.sh
if [ "$(id -u)" != 0 ]; then exec sudo "$0" "$@"; fi
DIR=/Library/Printers/P1102Native
if [ -x /Library/Printers/hp/laserjet/p1100_1560_1600series/hpRaster.bundle/Contents/MacOS/rastertozjs ]; then
    "$DIR/use-hp-driver.sh"
else
    users=$(grep -l "$DIR/rastertop1102" /etc/cups/ppd/*.ppd 2>/dev/null)
    [ -n "$users" ] && echo "Warning: these queues will have no driver: $(for f in $users; do basename "$f" .ppd; done | tr '\n' ' ')"
fi
rm -f "/Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1100 Native.ppd.gz"
rm -rf "$DIR"
pkgutil --forget local.p1102native.driver >/dev/null 2>&1
echo "Native P1100 driver removed."
