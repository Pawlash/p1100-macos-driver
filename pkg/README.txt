Native driver for the HP LaserJet Professional P1100 series
===========================================================

Verified on one printer only so far (a P1102 over USB).

Runs natively on Apple Silicon and Intel, without Rosetta. It replaces HP's
Intel-only "rastertozjs" filter and sends the printer exactly the same data as
HP's driver.

What the installer does
-----------------------
- Installs the driver in /Library/Printers/P1102Native
- Switches existing P1100 series queues that use HP's driver to this one
  (A4 by default, paper source "Automatically Select")
- For a new printer, add it in System Settings > Printers & Scanners. macOS
  selects "HP LaserJet Professional P1100, native driver" automatically.

The installer is not signed with an Apple developer certificate. If macOS
blocks it, go to System Settings > Privacy & Security > "Open Anyway".

Two-sided printing
------------------
The printer prints the back pages first, then pauses with the light blinking.
Take the stack out of the output bin without turning it, put it back into the
input tray printed side down, and the printer continues with the front pages.

Helper scripts (in Terminal)
----------------------------
  sudo /Library/Printers/P1102Native/use-native-driver.sh   switch queues to this driver
  sudo /Library/Printers/P1102Native/use-hp-driver.sh       switch back to HP's driver (needs Rosetta)
  sudo /Library/Printers/P1102Native/remove-hp-driver.sh    delete HP's old driver (/Library/Printers/hp)
  sudo /Library/Printers/P1102Native/uninstall.sh           remove this driver

Files
-----
  /Library/Printers/P1102Native/rastertop1102     the CUPS filter
  /Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1100 Native.ppd.gz
