#!/bin/sh
# Delete HP's old x86 driver once the native driver works:
#   - /Library/Printers/hp (filters, frameworks, utilities; all x86_64, need Rosetta)
#   - every PPD in /Library/Printers/PPDs that points into /Library/Printers/hp
#     (they cannot work without it; leaving them lets macOS pick a dead driver)
# Refuses if any printer queue still uses HP's files. Undo = reinstall HP's driver package.
#   sudo /Library/Printers/P1102Native/remove-hp-driver.sh [-y]
if [ "$(id -u)" != 0 ]; then exec sudo "$0" "$@"; fi
HPDIR=/Library/Printers/hp
PPDDIR=/Library/Printers/PPDs/Contents/Resources

users=$(grep -l "$HPDIR/" /etc/cups/ppd/*.ppd 2>/dev/null)
if [ -n "$users" ]; then
    echo "These queues still use HP's driver, switch them first (use-native-driver.sh):"
    for f in $users; do echo "  $(basename "$f" .ppd)"; done
    exit 1
fi

dead=$(mktemp /tmp/hpppd.XXXXXX)
for f in "$PPDDIR"/*; do
    [ -f "$f" ] || continue
    case $f in *.gz) cat=gzcat ;; *) cat=cat ;; esac
    $cat "$f" 2>/dev/null | grep -q "$HPDIR/" && echo "$f" >> "$dead"
done
n=$(wc -l < "$dead" | tr -d ' ')
[ -d "$HPDIR" ] && du -sh "$HPDIR"
echo "$n HP PPD files depend on it."
if [ ! -d "$HPDIR" ] && [ "$n" = 0 ]; then echo "Nothing to remove."; rm -f "$dead"; exit 0; fi

if [ "${1:-}" != "-y" ]; then
    printf "Delete %s and those %s PPD files? [y/N] " "$HPDIR" "$n"
    read -r a
    case $a in y|Y) ;; *) echo "Nothing removed."; rm -f "$dead"; exit 0 ;; esac
fi
while IFS= read -r f; do rm -f "$f"; done < "$dead"
rm -f "$dead"
rm -rf "$HPDIR"
rm -rf /Library/Printers/P1102Native/backup   # HP queue settings, useless without HP's driver
echo "HP driver removed."
