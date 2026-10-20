#!/usr/bin/env python3
"""Build an expected ZJS job out of page records taken from reference files.

  zjscompose.py OUT SPEC...
    SPEC is FILE:N (page N, 1-based, of FILE) or PAUSE.
    The job header/trailer are taken from the first FILE.

Used to check manual-duplex output for cases where HP's own filter crashes: every page of
our duplex job must be byte-identical to the same page printed one-sided by HP.
"""
import struct
import sys


def split(path):
    d = open(path, "rb").read()
    i = d.find(b"JZJZ") + 4
    size = struct.unpack(">I", d[i:i + 4])[0]
    head = d[:i + size]                     # PJL + JZJZ + START_DOC
    p = i + size
    pages, cur = [], None
    while d[p:p + 2] != b"\x1b%":
        size, typ = struct.unpack(">II", d[p:p + 8])
        rec = d[p:p + size]
        if typ == 2:
            cur = bytearray()
        if cur is not None:
            cur += rec
        if typ == 3:
            pages.append(bytes(cur))
            cur = None
        if typ == 1:
            tail = d[p:]
            break
        p += size
    return head, pages, tail


def main():
    out, specs = sys.argv[1], sys.argv[2:]
    cache = {}
    parts, head, tail = [], None, None
    for s in specs:
        if s == "PAUSE":
            parts.append(struct.pack(">IIIHH", 16, 11, 0, 0, 0x5a5a))
            continue
        f, n = s.rsplit(":", 1)
        if f not in cache:
            cache[f] = split(f)
        h, pages, t = cache[f]
        if head is None:
            head, tail = h, t
        parts.append(pages[int(n) - 1])
    open(out, "wb").write(head + b"".join(parts) + tail)


if __name__ == "__main__":
    main()
