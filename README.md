# Native macOS driver for the HP LaserJet Professional P1100 series

> **Note:** so far this has been verified on one printer only, a P1102 connected over USB. The other P1100 models should work the same way, but nobody has confirmed it yet. See [Supported printers](#supported-printers).

HP's macOS driver for the P1100 series (P1102, P1104, P1106, P1107, P1108, P1109) is an Intel-only binary. On Apple Silicon it only runs through Rosetta, and Apple is phasing Rosetta out after macOS 27. Once it's gone, the printer stops working even though the hardware is perfectly fine.

This project replaces the one part of HP's driver that matters, the CUPS filter `rastertozjs`. It's written from scratch in C and runs natively on Apple Silicon and Intel. Everything else is still handled by macOS as before.

The output sent to the printer is byte for byte identical to what HP's original driver sends. This was checked on 85 test cases: all paper sizes, all media types, both quality modes, EconoMode, density, copies, manual duplex and a set of difficult test images. Print quality is exactly the same as with HP's driver.

## Supported printers

| Printer | Status |
|---|---|
| HP LaserJet Professional **P1102** (USB) | Tested, works |
| P1104, P1106, P1107, P1108, P1109 | Same driver and PPD in HP's package, should work, not tested yet |
| P1102w and other "w" (Wi-Fi) models | HP uses a separate PPD for these, not generated here, unknown |
| P1566, P1606dn | Different code path in HP's driver, not supported |

Tested on macOS 27 on an Apple Silicon Mac.

If you try it on another model, please open an issue using the "printer report" template, whether it works or not. I'll update the table.

## Why there's no ready-made installer

The driver needs two small halftone tables (24 KB) from HP's driver, plus HP's PPD and printer icon. Those belong to HP, so they are not included in this repository or in any release. Sorry about that. Instead, the build script takes them from Apple's official HP driver package on your own machine. It's a single command.

## Requirements

- A Mac with Apple Silicon or Intel, macOS 11 or newer
- Xcode Command Line Tools (for `clang` and `python3`):
  ```
  xcode-select --install
  ```
  Nothing else is needed. No Homebrew, no extra libraries (CUPS is part of macOS).
- An internet connection for a one-time 584 MB download from Apple
- About 1.5 GB of free disk space while the package is unpacked

## Build and install

```
git clone https://github.com/Pawlash/p1100-macos-driver.git
cd p1100-macos-driver
./build.sh
open "dist/P1100 Series Native Driver.pkg"
```

The installer asks for your password, installs the driver and switches any existing P1100 series queue that uses HP's driver to this one. If the printer isn't set up yet, add it in System Settings > Printers & Scanners. macOS selects "HP LaserJet Professional P1100, native driver" automatically.

The installer is not signed with an Apple developer certificate. If macOS blocks it, go to System Settings > Privacy & Security and click "Open Anyway". When you build it yourself on the same Mac this usually doesn't happen.

## Where the HP files come from

`./build.sh` runs `tools/fetch_hp_files.sh`. It downloads Apple's official "HP Printer Drivers" package, unpacks it into a temporary folder (nothing is installed and Rosetta is not needed) and copies these files into `hp/`:

| File | Contents |
|---|---|
| `sdtahs10.bin`, `sdtahr10.bin` | The two halftone screens, taken from `rastertozjs` |
| `hp1100.ppd.gz` | HP's PPD, converted into this driver's PPD |
| `hp1100.icns` | The printer icon |

The package:

- Page: https://support.apple.com/kb/DL1888 ("HP 5.1.1 Printer Software Update")
- Direct link: https://updates.cdn-apple.com/2021/macos/071-46903-20211101-0BD2764A-901C-41BA-9573-C17B8FDC4D90/HewlettPackardPrinterDrivers.dmg
- File name: `HewlettPackardPrinterDrivers.dmg`
- Size: 584,435,157 bytes
- SHA-256: `523836b630431bc39b0170a17099099d6f821ef62ff29e6ec64ebb69b9954133`
- The .pkg inside is signed by "Apple Software"

Apple lists it as incompatible with macOS 12 and newer. That only applies to installing it. Here it's only unpacked to read a few files.

**If the link no longer works**, Apple has probably removed the file. You can look for `HewlettPackardPrinterDrivers.dmg` version 5.1.1 elsewhere (web archives, mirrors), but only use a copy whose SHA-256 matches the one above. Then point the build at it:

```
HP_DMG=/path/to/HewlettPackardPrinterDrivers.dmg ./build.sh
```

Each extracted file is also checked against a known hash, so a wrong or modified file makes the build stop.

### Manual extraction

If you prefer not to run the script, these are the steps it performs:

```
# 1. Download and verify
curl -L -o HewlettPackardPrinterDrivers.dmg "https://updates.cdn-apple.com/2021/macos/071-46903-20211101-0BD2764A-901C-41BA-9573-C17B8FDC4D90/HewlettPackardPrinterDrivers.dmg"
shasum -a 256 HewlettPackardPrinterDrivers.dmg
# expected: 523836b630431bc39b0170a17099099d6f821ef62ff29e6ec64ebb69b9954133

# 2. Mount it and unpack the package (this does not install anything)
mkdir mnt
hdiutil attach -nobrowse -readonly -mountpoint mnt HewlettPackardPrinterDrivers.dmg
pkgutil --expand-full mnt/HewlettPackardPrinterDrivers.pkg x
hdiutil detach mnt

# 3. Copy out the needed files
P=x/HewlettPackardPrinterDrivers.pkg/Payload/Library/Printers
BIN=$P/hp/laserjet/p1100_1560_1600series/hpRaster.bundle/Contents/MacOS/rastertozjs
mkdir -p hp
dd if=$BIN of=hp/sdtahs10.bin bs=1 skip=$((0x45940)) count=12288
dd if=$BIN of=hp/sdtahr10.bin bs=1 skip=$((0x42940)) count=12288
cp $P/PPDs/Contents/Resources/hp1100.ppd.gz $P/hp/Icons/hp1100.icns hp/
shasum -a 256 hp/*
```

Expected hashes:

```
4e57aeadfc9fd130ddadd2b9049c78f05a4ab35cd725f55db016cd5c1d4a5b50  hp/hp1100.icns
44e5ccb4e16a65fdc0f85fb50e391c3ea9cd3131abc1b6ebcf5acec206478d4c  hp/hp1100.ppd.gz
bee7e551104522f1ccb70f9d1808103d203bc9d97c4f2d1d38e6ffda3d6e2e51  hp/sdtahr10.bin
efa8a8e8c49da4f6383e20b8cd3187327f4be325dc888956aa631d61bed44cce  hp/sdtahs10.bin
```

For reference, `rastertozjs` itself is `be9415ca182fec58a17bea00603ff5591612d80b391299e307676c870eea0ca9`. After that, run `./build.sh`. It finds the files in `hp/` and skips the download. On macOS 27, hdiutil shows a deprecation warning for `-mountpoint`. It still works, or you can use `diskutil image attach --readOnly --mountPoint mnt ...` instead.

## Differences from HP's driver

- A4 is the default paper size (HP's PPD defaults to Letter).
- The paper source defaults to "Automatically Select". HP's PPD only offered "Manual Feed", where the printer waits for a sheet to be inserted after the job arrives. Paper already in the tray is ignored and the attention light comes on. "Manual Feed" is still available as an option.
- Printer status is shown in the print queue (out of paper, paper jam, door open, manual duplex pause), in English or Polish depending on the system language.
- When the printer runs out of paper, the queue retries the job every 30 seconds instead of pausing. Load paper, switch the printer off and on (the P1102 keeps blinking red after a paper-out until it is restarted) and the job prints by itself.
- Manual duplex works with an odd number of pages (HP's filter crashes in that case).
- A one-page job with two-sided printing enabled is printed one-sided instead of asking you to flip a single sheet.
- No ink level reporting and no HP Utility app. Both were Intel-only.

## Manual duplex

Enable two-sided printing as usual. The printer prints all back pages first, then pauses with the light blinking. Take the whole stack out of the output bin without turning it, put it back into the input tray printed side down, and the printer continues with the front pages.

## Helper scripts

After installation these are in `/Library/Printers/P1102Native/`:

```
sudo /Library/Printers/P1102Native/use-native-driver.sh   # switch P1100 queues to this driver
sudo /Library/Printers/P1102Native/use-hp-driver.sh       # switch back to HP's driver (only while Rosetta exists)
sudo /Library/Printers/P1102Native/remove-hp-driver.sh    # delete HP's old Intel driver (/Library/Printers/hp)
sudo /Library/Printers/P1102Native/uninstall.sh           # remove this driver
```

## How it works

The P1100 series printers use the ZjStream (ZJS) language: a PJL header, then for each page a header with the paper settings and the page image, compressed with JBIG (ITU-T T.82) at 2 bits per pixel. macOS renders each page to 8-bit grayscale. The filter halftones it with HP's 12x12 screens, JBIG-encodes it and wraps it in ZJS. Apart from the two screens, all of it is original code, based on analysing what HP's filter does and verified against its output.

`./build.sh --test` runs the comparison against HP's output. The reference files are HP's own output, so they aren't in the repository either. `tests/capture_ref.sh` generates them, but only while HP's driver and Rosetta still work.

## License

The code in this repository is released under the MIT License, see [LICENSE](LICENSE). It covers only the files in this repository, not the HP files the build downloads from Apple's package.

## Legal

This is a personal project, made to keep my own printer working. It is not affiliated with or endorsed by HP. HP and LaserJet are trademarks of HP Inc.

The code was written from scratch for interoperability with the printer, and the repository contains no HP code or data. The HP files the build needs are taken from HP's own driver package, which Apple distributes for free. For the same reason there is no prebuilt installer here, since it would include HP's files.

If HP has any objection to this project, please email me at the address on my GitHub profile and I will take the repository down.
