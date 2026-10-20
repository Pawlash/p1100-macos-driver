#!/usr/bin/env python3
"""Write an mtree manifest (for the cpio Payload) and an mkbom file list for ROOT.

  mkpayload.py ROOT OUT.mtree OUT.bomlist

Every file is root:wheel. Existing system directories get exactly the owner and mode they
have on a stock macOS, so installing the package never changes them. No extended
attributes or AppleDouble files end up in the package.
"""
import os
import subprocess
import sys
import time

SYSTEM_DIRS = {                       # path: (mode, uid, gid)
    ".": (0o755, 0, 0),
    "./Library": (0o755, 0, 0),
    "./Library/Printers": (0o555, 0, 0),
    "./Library/Printers/PPDs": (0o755, 0, 80),
    "./Library/Printers/PPDs/Contents": (0o755, 0, 80),
    "./Library/Printers/PPDs/Contents/Resources": (0o755, 0, 80),
}


def mtree_escape(p):
    return "".join(c if c.isalnum() or c in "./_-+" else "\\%03o" % ord(c) for c in p)


def main(root, mtree_out, bom_out):
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        rel = "." + dirpath[len(root):] if dirpath != root else "."
        entries.append((rel, None))
        for f in sorted(filenames):
            entries.append((rel + "/" + f, os.path.join(dirpath, f)))
    entries.sort(key=lambda e: e[0])
    mt, bl = ["#mtree"], []
    for rel, src in entries:
        if src is None:
            mode, uid, gid = SYSTEM_DIRS.get(rel, (0o755, 0, 0))
            mt.append("%s type=dir mode=%04o uid=%d gid=%d uname=%s gname=%s time=%d.0" % (
                mtree_escape(rel), mode, uid, gid, "root", "admin" if gid == 80 else "wheel",
                int(time.time())))
            bl.append("%s\t%o\t%d/%d" % (rel, 0o40000 | mode, uid, gid))
        else:
            mode = 0o755 if os.access(src, os.X_OK) else 0o644
            size = os.path.getsize(src)
            crc = subprocess.check_output(["cksum", src]).split()[0].decode()
            mt.append("%s type=file mode=%04o uid=0 gid=0 uname=root gname=wheel contents=%s" % (
                mtree_escape(rel), mode, mtree_escape(os.path.abspath(src))))
            bl.append("%s\t%o\t0/0\t%d\t%s" % (rel, 0o100000 | mode, size, crc))
    open(mtree_out, "w").write("\n".join(mt) + "\n")
    open(bom_out, "w").write("\n".join(bl) + "\n")
    print(len([e for e in entries if e[1]]), "files")


if __name__ == "__main__":
    main(*sys.argv[1:4])
