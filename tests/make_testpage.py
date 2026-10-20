#!/usr/bin/env python3
"""Write a one-page A4 test PDF (text sizes, hairlines, gray steps, gradient)."""
import sys
import zlib


def page_stream():
    s = []
    t = lambda x, y, size, txt: s.append("BT /F1 %g Tf %g %g Td (%s) Tj ET" % (size, x, y, txt))
    s.append("0 g")
    t(50, 790, 20, "HP LaserJet P1102 - native driver test page")
    t(50, 770, 10, "rastertop1102 1.0 - no Rosetta, no HP binaries")
    y = 740
    for size in (4, 5, 6, 7, 8, 10, 12, 14):
        t(50, y, size, "%gpt: The quick brown fox jumps over the lazy dog 0123456789" % size)
        y -= size + 6
    # hairlines
    y = 600
    for w in (0.1, 0.25, 0.5, 1, 2, 4):
        s.append("%g w 50 %g m 545 %g l S" % (w, y, y))
        t(50, y + 3, 6, "%gpt line" % w)
        y -= 16
    # gray steps
    t(50, 490, 10, "Gray steps 0-100%:")
    for i in range(11):
        g = 1 - i / 10
        s.append("%g g %g 420 45 60 re f" % (g, 50 + i * 45))
        s.append("0 g")
        t(55 + i * 45, 410, 7, "%d%%" % (i * 10))
    # smooth gradient
    t(50, 385, 10, "Gradient:")
    s.append("q 50 320 495 55 re W n /Sh1 sh Q")
    # fine patterns
    t(50, 295, 10, "Fine patterns:")
    for i in range(0, 100):
        x = 50 + i * 2.4
        s.append("0.3 w %g 200 m %g 285 l S" % (x, x))
    for i in range(0, 36):
        s.append("0.3 w 300 %g m 545 %g l S" % (200 + i * 2.4, 200 + i * 2.4))
    # circles (bezier approximation)
    for r in (20, 40, 60):
        k = 0.5523 * r
        cx, cy = 150, 110
        s.append("1 w %g %g m %g %g %g %g %g %g c %g %g %g %g %g %g c %g %g %g %g %g %g c "
                 "%g %g %g %g %g %g c S" % (
                     cx + r, cy, cx + r, cy + k, cx + k, cy + r, cx, cy + r,
                     cx - k, cy + r, cx - r, cy + k, cx - r, cy,
                     cx - r, cy - k, cx - k, cy - r, cx, cy - r,
                     cx + k, cy - r, cx + r, cy - k, cx + r, cy))
    s.append("0 g")
    t(260, 130, 9, "If text is sharp, lines are even and the gray steps")
    t(260, 118, 9, "get darker one by one, everything works.")
    t(260, 60, 7, "Top-left corner should be about 12pt from the paper edge.")
    return "\n".join(s).encode()


def main(path):
    content = zlib.compress(page_stream())
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R "
        b"/Resources << /Font << /F1 5 0 R >> /Shading << /Sh1 6 0 R >> >> >>",
        b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(content) + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [50 0 545 0] "
        b"/Function << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0] /N 1 >> >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offs = []
    for i, o in enumerate(objs, 1):
        offs.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    open(path, "wb").write(out)


if __name__ == "__main__":
    main(sys.argv[1])
