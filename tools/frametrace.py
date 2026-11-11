"""frametrace.py <trace.csv> [more.csv ...] [--from S] [--to S] [--spikes N] [--phases] [--csv out.csv]

Summarise FRAME_TRACE files (libs/video/rsx_d3d12_backend.c: one row per presented frame).

Per file, and per phase when the run had a scripted right stick (pad_rx: 128 = camera still, else
panning; each contiguous run of one stick value is a segment; --phases lists them):
  frames, mean fps, present-interval p50/p95/p99/p99.9/max, frames longer than 1.5x and 2x the
  median, the cost split of the render thread (walker idle = waiting for the guest, render_frame,
  GPU waits, the rest = command decode/record), per-frame event rates (shader compiles, pipeline
  creations, texture uploads, readbacks), and how many vblanks each displayed frame stayed on screen
  (from the DXGI frame statistics columns), which is what a viewer sees as judder; waits for a shader
  a compile worker was building and record-time compile kicks (SHADER_ASYNC).
--spikes N: the N longest frames with what happened in them.
--from/--to: seconds after READY (t_ready_s), default the whole run after READY.
--csv: append one summary row per (file, phase) to out.csv.
Standard library only.
"""
import csv
import math
import sys


def pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * p / 100.0
    f = math.floor(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    out = []
    for r in rows:
        try:
            out.append({k: float(v) for k, v in r.items() if v not in (None, "")})
        except ValueError:
            continue                          # a torn last line
    return out


def vblank_hist(rows):
    """Vblanks between successive displayed presents: {present_count: refresh} pairs from the
    statistics columns (each row reports the newest present the display has shown)."""
    seen = {}
    for r in rows:
        pc, pr = int(r.get("present_count", 0)), int(r.get("present_refresh", 0))
        if pc and pr and pc not in seen:
            seen[pc] = pr
    ks = sorted(seen)
    d = []
    for a, b in zip(ks, ks[1:]):
        if b == a + 1:
            d.append(seen[b] - seen[a])
    h = {}
    for x in d:
        h[x] = h.get(x, 0) + 1
    return d, h


def summary(rows, label):
    iv = [r["interval_ms"] for r in rows if r.get("interval_ms", 0) > 0]
    if len(iv) < 5:
        return None
    med = pct(iv, 50)
    tot = sum(iv)
    n = len(iv)
    def mean(k):
        return sum(r.get(k, 0.0) for r in rows) / len(rows)
    s = {
        "label": label, "frames": n, "fps": 1000.0 * n / tot if tot else 0.0,
        "p50": med, "p95": pct(iv, 95), "p99": pct(iv, 99), "p999": pct(iv, 99.9), "max": max(iv),
        "gt1.5x": sum(1 for x in iv if x > 1.5 * med), "gt2x": sum(1 for x in iv if x > 2 * med),
        "idle": mean("idle_ms"), "rf": mean("rf_ms"), "gpuwait": mean("gpuwait_ms"),
        "present": mean("present_ms"), "pace": mean("pace_ms"),
        "flip_dt_p50": pct([r["flip_dt_ms"] for r in rows if r.get("flip_dt_ms", 0) > 0], 50),
        "flip_dt_p99": pct([r["flip_dt_ms"] for r in rows if r.get("flip_dt_ms", 0) > 0], 99),
        "f2p": mean("flip_to_present_ms"),
        "draws": mean("draws"), "batches": mean("batches"), "fl_rb": mean("fl_readback"),
        "compiles": sum(r.get("compiles", 0) for r in rows), "compile_ms": sum(r.get("compile_ms", 0) for r in rows),
        "sc_hits": sum(r.get("sc_hits", 0) for r in rows),
        "vs_new": sum(r.get("vs_new", 0) for r in rows),
        "pso_creates": sum(r.get("pso_creates", 0) for r in rows),
        "pso_create_ms": sum(r.get("pso_create_ms", 0) for r in rows),
        "pso_ms": sum(r.get("pso_ms", 0) for r in rows),
        "tex_up": mean("tex_uploads"), "tex_new": sum(r.get("tex_new", 0) for r in rows),
        "tex_mb": mean("tex_mb"), "tex_ms": mean("tex_ms"), "texval_ms": mean("texval_ms"),
        "rt_new": sum(r.get("rt_new", 0) for r in rows), "rb": mean("readbacks"), "rb_ms": mean("readback_ms"),
        "vtx": mean("vtx_ms"),
        "sc_waits": sum(r.get("sc_waits", 0) for r in rows), "sc_wait_ms": sum(r.get("sc_wait_ms", 0) for r in rows),
        "kicks": sum(r.get("kicks", 0) for r in rows),
    }
    s["record"] = 1000.0 / s["fps"] - s["idle"] - s["rf"] if s["fps"] else 0.0
    d, h = vblank_hist(rows)
    s["vblanks"] = " ".join("%dx%d" % (k, h[k]) for k in sorted(h)) if h else "-"
    s["vb_changes"] = sum(1 for a, b in zip(d, d[1:]) if a != b)
    return s


def show(s):
    print("  %-22s %5d fr %6.1f fps | ms p50 %5.2f p95 %5.2f p99 %6.2f p99.9 %6.2f max %6.2f | >1.5x %3d >2x %3d"
          % (s["label"], s["frames"], s["fps"], s["p50"], s["p95"], s["p99"], s["p999"], s["max"], s["gt1.5x"], s["gt2x"]))
    print("  %-22s split ms/frame: idle %.2f  record %.2f  render_frame %.2f (gpu wait %.2f, present %.2f, pace %.2f)"
          " | flip dt p50 %.2f p99 %.2f | flip->present %.2f"
          % ("", s["idle"], s["record"], s["rf"], s["gpuwait"], s["present"], s["pace"], s["flip_dt_p50"],
             s["flip_dt_p99"], s["f2p"]))
    print("  %-22s draws %.0f batches %.2f (readback flushes %.2f) | compiles %d (%.1f ms) disk hits %d vs_new %d |"
          " pso creates %d (%.1f ms; miss path %.1f ms) | tex %.1f up/fr (%d new, %.2f MB/fr, %.2f ms/fr, validate %.2f)"
          " rt_new %d | readbacks %.2f/fr %.2f ms | vtx %.2f ms"
          % ("", s["draws"], s["batches"], s["fl_rb"], s["compiles"], s["compile_ms"], s["sc_hits"], s["vs_new"],
             s["pso_creates"], s["pso_create_ms"], s["pso_ms"], s["tex_up"], s["tex_new"], s["tex_mb"], s["tex_ms"],
             s["texval_ms"], s["rt_new"], s["rb"], s["rb_ms"], s["vtx"]))
    print("  %-22s vblanks per displayed frame: %s (pattern changes %d) | waits for a worker's compile %d (%.1f ms),"
          " record-time kicks %d" % ("", s["vblanks"], s["vb_changes"], s["sc_waits"], s["sc_wait_ms"], s["kicks"]))


def segments(rows):
    segs = []
    for r in rows:
        rx = int(r.get("pad_rx", -1))
        if not segs or segs[-1][0] != rx:
            segs.append([rx, []])
        segs[-1][1].append(r)
    return segs


def main(argv):
    files, a_from, a_to, nsp, phases, out = [], 0.0, 1e9, 0, False, None
    i = 0
    while i < len(argv):
        x = argv[i]
        if x == "--from": a_from = float(argv[i + 1]); i += 2
        elif x == "--to": a_to = float(argv[i + 1]); i += 2
        elif x == "--spikes": nsp = int(argv[i + 1]); i += 2
        elif x == "--phases": phases = True; i += 1
        elif x == "--csv": out = argv[i + 1]; i += 2
        else: files.append(x); i += 1
    allsum = []
    for p in files:
        rows = [r for r in load(p) if a_from <= r.get("t_ready_s", -1) <= a_to]
        print("%s: %d frames after READY" % (p, len(rows)))
        if not rows:
            continue
        sums = []
        s = summary(rows, "all"); sums.append(s)
        still = [r for r in rows if int(r.get("pad_rx", -1)) in (128, -1)]
        pan = [r for r in rows if int(r.get("pad_rx", -1)) not in (128, -1)]
        for lab, rr in (("camera still", still), ("camera panning", pan)):
            s = summary(rr, lab)
            if s: sums.append(s)
        if phases:
            k = {}
            for rx, rr in segments(rows):
                nm = "still" if rx in (128, -1) else ("rx=%d" % rx)
                k[nm] = k.get(nm, 0) + 1
                s = summary(rr, "%s #%d @%.0fs" % (nm, k[nm], rr[0]["t_ready_s"]))
                if s: sums.append(s)
        for s in sums:
            if s: show(s)
        allsum += [(p, s) for s in sums if s]
        if nsp:
            iv = sorted((r for r in rows if r.get("interval_ms", 0) > 0), key=lambda r: -r["interval_ms"])[:nsp]
            print("  longest frames:")
            for r in iv:
                print("   t=%7.2fs rx=%3d %6.2f ms | idle %.1f rf %.1f gw %.1f pres %.1f | draws %d batches %d rb %d (%.1f ms)"
                      " | comp %d (%.1f ms) vs %d pso %d (%.1f ms) | tex %d up (%d new, %.2f MB, %.1f ms) rt_new %d"
                      " | wait %d (%.1f ms) | flip_dt %.1f"
                      % (r["t_ready_s"], r.get("pad_rx", -1), r["interval_ms"], r.get("idle_ms", 0), r.get("rf_ms", 0),
                         r.get("gpuwait_ms", 0), r.get("present_ms", 0), r.get("draws", 0), r.get("batches", 0),
                         r.get("readbacks", 0), r.get("readback_ms", 0), r.get("compiles", 0), r.get("compile_ms", 0),
                         r.get("vs_new", 0), r.get("pso_creates", 0), r.get("pso_create_ms", 0), r.get("tex_uploads", 0),
                         r.get("tex_new", 0), r.get("tex_mb", 0), r.get("tex_ms", 0), r.get("rt_new", 0),
                         r.get("sc_waits", 0), r.get("sc_wait_ms", 0), r.get("flip_dt_ms", 0)))
    if out and allsum:
        keys = ["label", "frames", "fps", "p50", "p95", "p99", "p999", "max", "gt1.5x", "gt2x", "idle", "record", "rf",
                "gpuwait", "present", "pace", "flip_dt_p50", "flip_dt_p99", "f2p", "draws", "batches", "fl_rb", "compiles",
                "compile_ms", "sc_hits", "vs_new", "pso_creates", "pso_create_ms", "pso_ms", "tex_up", "tex_new", "tex_mb",
                "tex_ms", "texval_ms", "rt_new", "rb", "rb_ms", "vtx", "vblanks", "vb_changes", "sc_waits", "sc_wait_ms",
                "kicks"]
        new = False
        try:
            open(out).close()
        except OSError:
            new = True
        with open(out, "a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["file"] + keys)
            for p, s in allsum:
                w.writerow([p] + [("%.3f" % s[k]) if isinstance(s[k], float) else s[k] for k in keys])


if __name__ == "__main__":
    main(sys.argv[1:])
