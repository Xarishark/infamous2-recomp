"""bandstep.py <image.png> x0,y0,x1,y1 [--scale720]

Sky-banding measure matching hwtruth/visual (bandProfile): for each row of the
region, the mean luma over the region's columns; then the absolute
row-to-row steps. Prints median / p95 / max step and the count of steps
>= 2 and >= 4 levels. Hardware target (720p native): max step about 1-2
levels, median well under 1 (LACRY 2:38, 1440p: median 0.18, max 1.04).
Also reports per-column run lengths of identical luma (long flat runs followed
by a jump = quantisation banding). Read-only.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngstat import read_png


def main():
    w, h, bpp, rows = read_png(sys.argv[1])
    x0, y0, x1, y1 = (int(v) for v in sys.argv[2].split(','))
    if '--scale720' in sys.argv:
        sx, sy = w / 1280.0, h / 720.0
        x0, x1 = int(x0 * sx), int(x1 * sx); y0, y1 = int(y0 * sy), int(y1 * sy)
    lum_rows = []
    for y in range(y0, y1):
        r = rows[y]; s = 0.0
        for x in range(x0, x1):
            o = x * bpp; s += 0.2126 * r[o] + 0.7152 * r[o + 1] + 0.0722 * r[o + 2]
        lum_rows.append(s / (x1 - x0))
    steps = sorted(abs(lum_rows[i + 1] - lum_rows[i]) for i in range(len(lum_rows) - 1))
    n = len(steps)
    print('image %dx%d region %s rows %d' % (w, h, (x0, y0, x1, y1), len(lum_rows)))
    print('row-mean luma steps: median %.2f p95 %.2f max %.2f  >=2: %d  >=4: %d' % (
        steps[n // 2], steps[min(n - 1, int(n * 0.95))], steps[-1],
        sum(1 for s in steps if s >= 2), sum(1 for s in steps if s >= 4)))
    # per-column flat-run analysis on the green channel (most of luma)
    runs = []
    for x in range(x0, x1, max(1, (x1 - x0) // 16)):
        prev, run = None, 0
        for y in range(y0, y1):
            g = rows[y][x * bpp + 1]
            if g == prev: run += 1
            else:
                if prev is not None: runs.append(run)
                prev, run = g, 1
    runs.sort()
    if runs:
        print('green flat-run length: median %d p95 %d max %d (long runs + 1-level jumps = banding)' % (
            runs[len(runs) // 2], runs[min(len(runs) - 1, int(len(runs) * 0.95))], runs[-1]))


if __name__ == '__main__':
    main()
