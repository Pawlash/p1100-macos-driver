#!/usr/bin/env python3
"""Dump the structure of a ZjStream (ZJS) print file: PJL header, records, items."""
import struct
import sys

TYPES = {0: "START_DOC", 1: "END_DOC", 2: "START_PAGE", 3: "END_PAGE",
         4: "JBIG_BIH", 5: "JBIG_BID", 6: "END_JBIG", 7: "SIGNATURE",
         8: "RAW_IMAGE", 9: "START_PLANE", 10: "END_PLANE"}


def dump(path, show_data=False):
    d = open(path, "rb").read()
    i = d.find(b"JZJZ")
    if i < 0:
        sys.exit("no JZJZ signature")
    print("PJL header:", repr(d[:i]))
    p = i + 4
    nbid = 0
    bidbytes = 0
    while p < len(d):
        if d[p:p + 2] == b"\x1b%":
            print("trailer @%d:" % p, repr(d[p:]))
            break
        size, typ, items, res, sig = struct.unpack(">IIIHH", d[p:p + 16])
        name = TYPES.get(typ, "?%d" % typ)
        body = d[p + 16:p + size]
        if typ == 5:
            nbid += 1
            bidbytes += len(body)
            if show_data:
                print("  BID size=%d res=%d %s" % (size, res, body[:16].hex()))
        else:
            if nbid:
                print("  ... %d BID records, %d bytes" % (nbid, bidbytes))
                nbid = bidbytes = 0
            print("@%d %s size=%d items=%d res=0x%x sig=%s" % (p, name, size, items, res, hex(sig)))
            q = 0
            for _ in range(items):
                isz, iid, ityp, ipar = struct.unpack(">IHBB", body[q:q + 8])
                val = body[q + 8:q + isz]
                v = struct.unpack(">I", val)[0] if len(val) == 4 else val.hex()
                print("    item 0x%02x type=%d param=%d value=%s" % (iid, ityp, ipar, v))
                q += isz
            rest = body[q:]
            if rest:
                if typ == 4 and len(rest) == 20:
                    dl, dd, pp, _, xd, yd, l0, mx, my, order, opt = struct.unpack(">BBBBIIIBBBB", rest)
                    print("    BIH DL=%d D=%d P=%d XD=%d YD=%d L0=%d MX=%d MY=%d order=0x%x options=0x%x"
                          % (dl, dd, pp, xd, yd, l0, mx, my, order, opt))
                else:
                    print("    data:", rest[:64].hex(), "(%d bytes)" % len(rest))
        p += size


if __name__ == "__main__":
    dump(sys.argv[1], "-v" in sys.argv)
