"""halo.py <image.png> cx,cy [--scale720]

Radial luma profile around a light (lamp core) for the bloom-footprint check.
Prints mean luma on rings r = 0,4,8,12,16,20,24,32,40,48,64 (in 720p pixels;
with --scale720 the centre and radii are scaled to the image) and the radius
where the profile falls to half of (peak + background) / the background
(the ring mean at r=64). Hardware target (FRW 3:44, two lamps): half height at
about 16-20 px, background by about 40-50 px; the white core is only a few px.
Read-only.
"""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngstat import read_png

RADII = [0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64]


def main():
    w, h, bpp, rows = read_png(sys.argv[1])
    cx, cy = (float(v) for v in sys.argv[2].split(','))
    s = 1.0
    if '--scale720' in sys.argv:
        s = w / 1280.0; cx *= s; cy *= h / 720.0
    prof = []
    for r in RADII:
        rr = r * s; acc = 0.0; n = 0
        steps = max(8, int(2 * math.pi * max(rr, 1)))
        for k in range(steps):
            a = 2 * math.pi * k / steps
            x = int(round(cx + rr * math.cos(a))); y = int(round(cy + rr * math.sin(a)))
            if 0 <= x < w and 0 <= y < h:
                o = x * bpp; p = rows[y]
                acc += 0.2126 * p[o] + 0.7152 * p[o + 1] + 0.0722 * p[o + 2]; n += 1
        prof.append(acc / max(n, 1))
    print(' '.join('%d:%.0f' % (r, v) for r, v in zip(RADII, prof)))
    peak, bg = prof[0], prof[-1]
    half = bg + (peak - bg) / 2.0
    rhalf = next((r for r, v in zip(RADII, prof) if v <= half), None)
    rbg = next((r for r, v in zip(RADII, prof) if v <= bg + 0.1 * (peak - bg)), None)
    print('peak %.0f background %.0f  half-height radius %s px  ~background radius %s px (720p units)' % (
        peak, bg, rhalf, rbg))


if __name__ == '__main__':
    main()
