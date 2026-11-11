"""pngstat.py <image.png> [name=x0,y0,x1,y1 ...] [--scale720]

Stdlib-only PNG reader (8-bit RGB/RGBA, non-interlaced) that prints, per
region, the measures used by hwtruth/visual/targets.json:
  mean RGB, luma (Rec.709) p50/p95/max, hiPct (% luma >= 235),
  clipPct (% pixels with R,G,B all >= 250), R/G and G/B ratios, hue (deg).
Region coordinates are in 1280x720 space when --scale720 is given (they are
scaled to the image size), else in image pixels. With no regions, the whole
frame is measured. Read-only; writes nothing.
"""
import sys, zlib, struct, colorsys


def read_png(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, w = 8, [], None
    while pos < len(b):
        ln, = struct.unpack('>I', b[pos:pos + 4]); typ = b[pos + 4:pos + 8]
        data = b[pos + 8:pos + 8 + ln]; pos += 12 + ln
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', data)
            assert depth == 8 and ctype in (2, 6) and inter == 0, 'unsupported PNG'
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(data)
        elif typ == b'IEND':
            break
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            c = prev[i - bpp] if i >= bpp else 0
            up = prev[i]
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + up) & 255
            elif f == 3: line[i] = (line[i] + ((a + up) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(up - c), abs(a - c), abs(a + up - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (up if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line); prev = line
    return w, h, bpp, rows


def stats(w, h, bpp, rows, box):
    x0, y0, x1, y1 = box
    px = []
    for y in range(max(0, y0), min(h, y1)):
        r = rows[y]
        for x in range(max(0, x0), min(w, x1)):
            o = x * bpp; px.append((r[o], r[o + 1], r[o + 2]))
    n = len(px)
    if not n:
        return None
    mean = [sum(p[i] for p in px) / n for i in range(3)]
    lum = sorted(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] for p in px)
    hi = sum(1 for l in lum if l >= 235) * 100.0 / n
    clip = sum(1 for p in px if min(p) >= 250) * 100.0 / n
    hue = colorsys.rgb_to_hsv(*(c / 255.0 for c in mean))[0] * 360.0
    return {'n': n, 'mean': [round(c) for c in mean], 'l50': round(lum[n // 2]),
            'l95': round(lum[min(n - 1, int(n * 0.95))]), 'lmax': round(lum[-1]),
            'hiPct': round(hi, 2), 'clipPct': round(clip, 2),
            'R/G': round(mean[0] / max(mean[1], 1e-6), 2),
            'G/B': round(mean[1] / max(mean[2], 1e-6), 2), 'hue': round(hue, 1)}


def main():
    path = sys.argv[1]
    scale = '--scale720' in sys.argv
    w, h, bpp, rows = read_png(path)
    print('image %dx%d' % (w, h))
    regs = [a for a in sys.argv[2:] if '=' in a]
    if not regs:
        regs = ['whole=0,0,%d,%d' % ((1280, 720) if scale else (w, h))]
    for a in regs:
        name, coords = a.split('=', 1)
        x0, y0, x1, y1 = (int(v) for v in coords.split(','))
        if scale:
            sx, sy = w / 1280.0, h / 720.0
            x0, x1 = int(x0 * sx), int(x1 * sx); y0, y1 = int(y0 * sy), int(y1 * sy)
        print(name, stats(w, h, bpp, rows, (x0, y0, x1, y1)))


if __name__ == '__main__':
    main()
