#!/usr/bin/env python3
"""Derive the native-driver PPDs from HP's hp1100.ppd / hp1100w.ppd.

  make_ppd.py SRC.ppd[.gz] OUT.ppd

Changes: our filter instead of HP's x86 binaries, no x86 utility / ink-level command
filter, A4 as the default paper size, our own model name so both drivers can coexist.
"""
import gzip
import re
import sys

FILTER = "/Library/Printers/P1102Native/rastertop1102"
ICON = "/Library/Printers/P1102Native/printer.icns"
VERSION = "1.3"
AUTO = {"da": "Automatisk", "de": "Automatisch", "es": "Automático", "fi": "Automaattinen",
        "fr": "Automatique", "it": "Automatico", "ko": "자동", "nl": "Automatisch",
        "no": "Automatisk", "sv": "Automatiskt", "pt": "Automático", "zh_CN": "自动",
        "zh_TW": "自動"}


def main(src, dst):
    raw = gzip.open(src).read() if src.endswith(".gz") else open(src, "rb").read()
    text = raw.decode("latin-1")
    out = []
    for line in text.split("\n"):
        if line.startswith("*cupsFilter:") and "vnd.cups-command" in line:
            continue                                   # usbink (x86, ink level only)
        if line.startswith("*cupsCommands:") or line.startswith("*APPrinterUtilityPath:"):
            continue
        if line.startswith("*cupsFilter:") and "vnd.cups-raster" in line:
            line = '*cupsFilter: "application/vnd.cups-raster 100 %s"' % FILTER
        elif line.startswith("*APPrinterIconPath:"):
            line = '*APPrinterIconPath: "%s"' % ICON
        elif line.startswith("*PCFileName:"):
            line = re.sub(r'"hp(1100w?)\.ppd"', lambda m: '"N%s.PPD"' % m.group(1).upper(), line)
        elif line.startswith("*FileVersion:"):
            line = '*FileVersion: "%s"' % VERSION
        elif line.startswith("*ModelName:"):
            line = line[:-1] + ' Native"'
        elif line.startswith("*ShortNickName:"):
            line = line[:-1] + ' Native"'
        elif line.startswith("*NickName:"):
            line = re.sub(r', 6\.9"$', ', native driver %s"' % VERSION, line)
        elif re.match(r"\*Default(PageSize|PageRegion|ImageableArea|PaperDimension): Letter$", line):
            line = line.replace("Letter", "A4")
        elif line == "*DefaultInputSlot: Manual":
            # HP only offers "Manual Feed" (tray 4): the printer then waits for a sheet to be
            # inserted after the job arrives and ignores paper already in the tray (red LED).
            # MediaPosition 0 -> tray 7 (auto) picks up the loaded paper right away.
            line = "*DefaultInputSlot: Auto"
            out.append(line)
            line = '*InputSlot Auto/Automatically Select: "<</MediaPosition 0>>setpagedevice"'
        elif re.match(r"\*[a-zA-Z_]+\.Translation (Short)?NickName/", line):
            line = line.replace(", 6.9", ", native driver %s" % VERSION).replace(
                "P1100:", "P1100 Native:")
        m = re.match(r"\*([a-zA-Z_]+)\.InputSlot Manual/", line)
        if m:
            out.append(line)
            word = AUTO.get(m.group(1), "Automatically Select")
            # the file is UTF-8 inside a latin-1 declared PPD, like HP's translations
            line = '*%s.InputSlot Auto/%s: ""' % (m.group(1), word.encode().decode("latin-1"))
        out.append(line)
        if line.startswith("*%%%% Created by the CUPS PPD Compiler"):
            out.append("*%%%% Modified for the native P1100 series driver (rastertop1102): "
                       "no HP binaries, no Rosetta.")
    data = "\n".join(out).encode("latin-1")
    assert b"/Library/Printers/hp" not in data, "HP path left in PPD"
    open(dst, "wb").write(data)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
