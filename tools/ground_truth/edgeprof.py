"""edgeprof.py <image.png|.bmp> [name=x0,y0,x1,y1 ...] [--t 16] [--e 24] [--chan r|g|b] [--scale720] [--mark out.png]

Edge-outlier metric for shadow edges (stdlib only; PNG reader from pngstat.py,
24/32-bit BMP for the RSX_CAP render-target dumps). Per region, on Rec.709
luma (or one channel with --chan, e.g. the shadow mask's R):
  edge px   pixels on an edge: the 5x5 luma range around them is >= E
  spikes    isolated one-pixel outliers on an edge: along x or y, the pixel
            differs from BOTH neighbours by more than T in the same direction
            while the two neighbours differ from each other by less than T --
            a dot or a one-pixel notch that a filtered (PCF) edge cannot make
  jumps     one-pixel steps inside a ramp: along x or y, |L(p+1) - L(p)| > 2T
            while the steps either side, |L(p) - L(p-1)| and |L(p+2) - L(p+1)|,
            are both below T/2 -- a riser that skips the ramp
  per 1k    (spikes + jumps) per 1000 edge pixels
Region coordinates are image pixels, or 1280x720 space with --scale720. With
no region, the whole image. --mark writes a PNG copy with the outliers in
magenta (spikes) and cyan (jumps). Read-only on its input.

Use: the same crop of the same moment (a deterministic cutscene shot) with
two settings, e.g. ZTEX_PCF=gather vs the default; the difference in the
outlier count is what the setting changed, texture detail being common to both.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngstat import read_png  # noqa: E402


def read_bmp(path):
    """Uncompressed 24/32-bit BMP (the RSX_CAP render-target dumps) as read_png's (w, h, bpp, rows), RGB order."""
    import struct
    b = open(path, 'rb').read()
    assert b[:2] == b'BM', 'not a BMP'
    off, = struct.unpack('<I', b[10:14])
    w, h, planes, bits, comp = struct.unpack('<iiHHI', b[18:34])
    assert bits in (24, 32) and comp in (0, 3), 'unsupported BMP'
    bpp = bits // 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        line = b[off + sy * stride: off + sy * stride + w * bpp]
        out = bytearray(w * bpp)
        for x in range(w):
            o = x * bpp
            out[o] = line[o + 2]; out[o + 1] = line[o + 1]; out[o + 2] = line[o]
            if bpp == 4: out[o + 3] = line[o + 3]
        rows.append(out)
    return w, abs(h), bpp, rows


def luma_plane(w, h, bpp, rows):
    out = []
    for y in range(h):
        r = rows[y]
        out.append([0.2126 * r[x * bpp] + 0.7152 * r[x * bpp + 1] + 0.0722 * r[x * bpp + 2] for x in range(w)])
    return out


def measure(L, w, h, box, T, E, marks):
    x0, y0, x1, y1 = box
    x0, y0, x1, y1 = max(2, x0), max(2, y0), min(w - 3, x1), min(h - 3, y1)
    edge = spikes = jumps = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            lo = hi = L[y][x]
            for yy in range(y - 2, y + 3):
                row = L[yy]
                for xx in range(x - 2, x + 3):
                    v = row[xx]
                    if v < lo: lo = v
                    if v > hi: hi = v
            if hi - lo < E:
                continue
            edge += 1
            c = L[y][x]
            for dx, dy in ((1, 0), (0, 1)):
                a = L[y - dy][x - dx]; b = L[y + dy][x + dx]
                if abs(a - b) < T and ((c - a > T and c - b > T) or (a - c > T and b - c > T)):
                    spikes += 1; marks[(x, y)] = 's'
                n2 = L[y + 2 * dy][x + 2 * dx]
                if abs(b - c) > 2 * T and abs(c - a) < T / 2 and abs(n2 - b) < T / 2:
                    jumps += 1; marks.setdefault((x, y), 'j')
    return edge, spikes, jumps


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return 2
    path = args.pop(0)
    T, E, scale, mark, chan = 16.0, 24.0, False, None, None
    regions = []
    while args:
        a = args.pop(0)
        if a == '--t': T = float(args.pop(0))
        elif a == '--chan': chan = 'rgb'.index(args.pop(0))
        elif a == '--e': E = float(args.pop(0))
        elif a == '--scale720': scale = True
        elif a == '--mark': mark = args.pop(0)
        else:
            n, c = a.split('=')
            regions.append((n, [int(v) for v in c.split(',')]))
    w, h, bpp, rows = read_bmp(path) if path.lower().endswith('.bmp') else read_png(path)
    if chan is not None:                     # one channel (e.g. a shadow mask in R) instead of luma
        L = [[float(rows[y][x * bpp + chan]) for x in range(w)] for y in range(h)]
    else:
        L = luma_plane(w, h, bpp, rows)
    if not regions:
        regions = [('frame', [0, 0, w, h])]
    marks = {}
    print('%s %dx%d T=%g E=%g' % (os.path.basename(path), w, h, T, E))
    for n, b in regions:
        if scale:
            b = [int(round(b[0] * w / 1280)), int(round(b[1] * h / 720)), int(round(b[2] * w / 1280)),
                 int(round(b[3] * h / 720))]
        e, s, j = measure(L, w, h, b, T, E, marks)
        print('  %-12s box=%s edge px %6d  spikes %5d  jumps %5d  per 1k %.2f' %
              (n, ','.join(map(str, b)), e, s, j, 1000.0 * (s + j) / e if e else 0.0))
    if mark:                                 # an RGB PNG: the image with the outliers painted
        import struct
        import zlib
        raw = bytearray()
        for y in range(h):
            r = rows[y]
            raw.append(0)
            for x in range(w):
                m = marks.get((x, y))
                if m == 's': raw += b'\xff\x00\xff'
                elif m == 'j': raw += b'\x00\xff\xff'
                else: raw += bytes((r[x * bpp], r[x * bpp + 1], r[x * bpp + 2]))

        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
        with open(mark, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
