---
name: Printer report
about: Let others know whether the driver works on your printer
title: "[printer] <model> on macOS <version>"
labels: printer-report
---

**Printer model** (as printed on the front, e.g. P1102, P1106, P1102w):

**Connection** (USB / Wi-Fi / network):

**Mac** (Apple Silicon or Intel) and **macOS version**:

**Does it print?** yes / no

What did you try? Tick what worked:

- [ ] Regular page (text, images)
- [ ] A4 / Letter
- [ ] Two-sided printing (manual duplex)
- [ ] EconoMode
- [ ] "Out of paper" message shown in the print queue

**If something went wrong**, what happened (printer lights, messages on the Mac)? The end of the CUPS log helps a lot:

```
tail -n 100 /var/log/cups/error_log
```

For more detail, run `sudo cupsctl --debug-logging`, print again, paste the log, then run `sudo cupsctl --no-debug-logging`.
