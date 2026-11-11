#!/usr/bin/env python3
"""
play_triage.py -- read play-session issues files (runtime/diag/play_issues.c) and rank what to fix.

    py tools/play_triage.py                 the newest logs/play-*-issues.txt
    py tools/play_triage.py --all           every session in logs/, oldest first
    py tools/play_triage.py FILE [FILE...]  these files (yours, or ones other players sent)
    py tools/play_triage.py --json ...      the same, as JSON

For each session: the machine, how it ended, and the problems ranked by how much a player felt them
(crashes, frames that stopped, freezes and hitches grouped by cause, slow stretches, memory pressure,
log errors, the moments the player marked), each with where in the code to start. With several files
from different machines it adds a comparison, so a problem only one machine has stands out.
"""
import argparse
import glob
import json
import os
import re
import sys
from collections import Counter, defaultdict

LINE = re.compile(r"^\[(\d+):(\d\d):(\d\d)\.(\d)\] (\S+)\s+(.*)$")
TRAILER = re.compile(r"\{([^{}]*)\}\s*$")
CTX_EVENT = re.compile(r"last game event (\S+) ([\d.]+) s before")

# where to start looking, per cause (the {cause=...} keys play_issues.c writes)
WHERE = {
    "shader-compile": "shader compiles on the render thread: SHADER_PREWARM list / SHADER_KICK coverage "
                      "(libs/video/rsx_shader_async.h); FRAME_TRACE=<csv> + tools/frametrace.py for the frame",
    "pipeline": "pipeline-state creation: PSO cache keys and pre-creation (vp_get_fp_pso_impl, rsx_d3d12_backend.c)",
    "readback": "GPU -> guest readbacks the game waits on: REPORT_* / RB_SEQ (libs/video/rsx_report.c, cellGcmSys.c)",
    "gpu": "GPU-bound frames: GPU_TIME=1 for per-list GPU time; resolution scale, the heaviest passes",
    "present": "Present() blocking: swap-chain latency / vsync / FPS cap pacing (gcm_flip_pace, present path)",
    "game": "the game's own threads (PPU/SPU) late with the frame: threadcpu.ps1 / loadprof for which thread; "
            "streaming loads (Edge zlib inflate) if a load event is near",
    "render": "render-thread batch work outside compiles: FRAME_TRACE rf_ms vs its parts; texture validation",
    "record": "render thread outside the timed parts: the FIFO walker (vertex conversion), window messages, or "
              "Windows descheduling the thread -- FRAME_TRACE idle_ms/vtx_ms, WALKER_STATS=1",
    "texture": "texture streaming: uploads per frame (tc / vp_upload_tex_slot), new vs re-created textures",
    "unexplained": "time spread over many parts: FRAME_TRACE=<csv> around this moment",
}


def parse(path):
    s = {"file": os.path.basename(path), "path": path, "header": {}, "settings": [], "events": [],
         "end": None, "end_lines": []}
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = [l.rstrip("\r\n") for l in f]
    in_header, last_key, in_end = True, None, False
    for l in lines:
        if in_header:
            if l.startswith("Times are since"):
                in_header = False
                continue
            m = re.match(r"^(\w+)\s{2,}(.*)$", l)
            if m and not l.startswith("---"):
                last_key = m.group(1)
                if last_key == "settings":
                    s["settings"].append(m.group(2))
                else:
                    s["header"][last_key] = m.group(2)
            elif l.startswith(" " * 10) and last_key == "settings":
                s["settings"].append(l.strip())
            continue
        m = LINE.match(l)
        if m:
            h, mi, se, ds, kind, text = m.groups()
            t = int(h) * 3600 + int(mi) * 60 + int(se) + int(ds) / 10
            ev = {"t": t, "kind": kind, "text": text, "kv": {}}
            tm = TRAILER.search(text)
            if tm:
                for part in tm.group(1).split():
                    if "=" in part:
                        k, v = part.split("=", 1)
                        ev["kv"][k] = v
                ev["text"] = text[:tm.start()].rstrip()
            cm = CTX_EVENT.search(text)
            if cm:
                ev["last_event"] = cm.group(1)
                ev["event_age"] = float(cm.group(2))
            ev["paused"] = "game paused" in text
            ev["background"] = "window in the background" in text
            s["events"].append(ev)
            in_end = kind == "END"
            if in_end:
                s["end"] = ev
        elif in_end and l.startswith(" "):
            s["end_lines"].append(l.strip())
    return s


def fmt_t(t):
    t = int(t)
    return f"{t // 3600}:{t // 60 % 60:02d}:{t % 60:02d}"


def triage(s):
    ev = s["events"]
    out = {"file": s["file"], "machine": s["header"], "settings": s["settings"], "problems": []}
    dur = max((e["t"] for e in ev), default=0)
    out["duration"] = dur
    if s["end"]:
        out["ended"] = s["end"]["text"]
    else:
        out["ended"] = ("no END line: the game did not quit normally (crashed without a handler, killed from "
                        "Task Manager, or the PC lost power) -- or it is still running")
    minutes = [e for e in ev if e["kind"] == "MINUTE"]
    fps = [float(m.group(1)) for e in minutes for m in [re.search(r": ([\d.]+) fps", e["text"])] if m]
    out["fps_by_minute"] = fps

    probs = out["problems"]

    def add(rank, title, detail, where=None, times=None):
        probs.append({"rank": rank, "title": title, "detail": detail, "where": where,
                      "times": [fmt_t(t) for t in (times or [])[:8]]})

    for e in ev:
        if e["kind"] == "CRASH":
            add(1000, "CRASH", e["text"], "the minidump beside the issues file (WinDbg / VS); the module+offset in "
                "the line maps into build/infamous2.exe with its .pdb", [e["t"]])
    stalls = [e for e in ev if e["kind"] == "STALL"]
    if stalls:
        frozen = [e for e in stalls if "may have frozen" in e["text"]]
        add(900 if frozen else 500, f"frames stopped ({len(stalls)} times{', the game may have frozen' if frozen else ''})",
            "; ".join(sorted({e.get('last_event', '?') for e in stalls})) and
            "last game events: " + ", ".join(f"{k} x{v}" for k, v in Counter(e.get("last_event", "?") for e in stalls).most_common(5)),
            "a hang: the guest threads' state at that time (PPU_RESUME / ss census tools, or attach a debugger "
            "while it is stuck); a load: the loading path", [e["t"] for e in stalls])

    bursts = [e for e in ev if e["kind"] in ("STUTTER", "HITCH", "FREEZE")]
    by_cause = defaultdict(list)
    for e in bursts:
        by_cause[e["kv"].get("cause", "unexplained")].append(e)
    for cause, es in by_cause.items():
        n_frames = sum(int(e["kv"].get("n", 1)) for e in es)
        worst = max(float(e["kv"].get("worst", 0)) for e in es)
        kinds = Counter(e["kind"] for e in es)
        weight = sum({"STUTTER": 8, "HITCH": 32, "FREEZE": 96}[e["kind"]] // (2 if (e["paused"] or e["background"]) else 1) for e in es)
        paused = sum(1 for e in es if e["paused"])
        bg = sum(1 for e in es if e["background"])
        evs = Counter(e.get("last_event", "?") for e in es if not e["paused"])
        recent = Counter(e.get("last_event", "?") for e in es if e.get("event_age", 99) < 2.0)
        detail = (f"{len(es)} lines ({', '.join(f'{v} {k.lower()}' for k, v in kinds.most_common())}), "
                  f"{n_frames} slow frames, worst {worst:.0f} ms")
        if paused:
            detail += f"; {paused} while the game was paused"
        if bg:
            detail += f"; {bg} with the window in the background (alt-tab: usually not a real problem)"
        if recent:
            detail += "; within 2 s of: " + ", ".join(f"{k} x{v}" for k, v in recent.most_common(4))
        elif evs:
            detail += "; last game events: " + ", ".join(f"{k} x{v}" for k, v in evs.most_common(3))
        add(150 + weight, f"slow frames: {cause}", detail, WHERE.get(cause),
            [e["t"] for e in sorted(es, key=lambda e: -float(e["kv"].get("worst", 0)))])

    slows = [e for e in ev if e["kind"] == "SLOW"]
    if slows:
        tot = sum(float(e["kv"].get("dur", 0)) for e in slows)
        avg = sum(float(e["kv"].get("avg", 0)) * float(e["kv"].get("dur", 0)) for e in slows) / tot if tot else 0
        tgt = slows[0]["kv"].get("target", "?")
        add(120 + tot / 5, f"under the {tgt} fps target for {tot:.0f} s ({len(slows)} stretches)",
            f"{avg:.1f} fps on average there",
            "the walker profile at those places (campaign.sh / perf_report4.py), threadcpu.ps1 for the bound thread",
            [e["t"] for e in slows])

    for e in ev:
        if e["kind"] == "NOTE":
            r = 300 if ("over budget" in e["text"] or "low on system memory" in e["text"]) else 150
            add(r, "note", e["text"], None, [e["t"]])
        if e["kind"] == "WARN":
            add(120, "warning", e["text"], None, [e["t"]])

    errs = [e for e in ev if e["kind"] == "LOG-ERR"]
    warns = [e for e in ev if e["kind"] == "LOG"]
    repeats = {}
    for l in s["end_lines"]:
        m = re.match(r"^x(\d+)\s+(.*)$", l)
        if m:
            repeats[m.group(2)] = int(m.group(1))
    for e in errs:
        add(140, "log error", e["text"] + (f"  (x{repeats[e['text']]})" if e["text"] in repeats else ""),
            "grep the play log for it; the module named in its [tag]", [e["t"]])
    for e in warns:
        add(60, "log warning", e["text"] + (f"  (x{repeats[e['text']]})" if e["text"] in repeats else ""), None, [e["t"]])

    for e in ev:
        if e["kind"] == "MARK":
            near = [b for b in bursts if abs(b["t"] - e["t"]) < 10]
            d = e["text"] + (f" -- {len(near)} slow-frame line(s) within 10 s" if near else
                             " -- no slow frames near it: likely a visual problem (see the screenshot)")
            add(700, "the player marked this", d, "the screenshot beside the issues file", [e["t"]])

    probs.sort(key=lambda p: -p["rank"])
    return out


def print_report(r):
    m = r["machine"]
    print(f"## {r['file']}")
    print(f"- machine: {m.get('cpu', '?')} | {m.get('gpu', '?')} | {m.get('memory', '?')} | {m.get('display', '?')}"
          + (f" | power: {m['power']}" if "power" in m else ""))
    print(f"- build: {m.get('build', '?')} | started {m.get('started', '?')} | length {fmt_t(r['duration'])}")
    print(f"- ended: {r['ended']}")
    if r["settings"]:
        print(f"- settings: {' '.join(r['settings'])}")
    if r["fps_by_minute"]:
        f = r["fps_by_minute"]
        print(f"- fps by minute: min {min(f):.1f}, median {sorted(f)[len(f) // 2]:.1f}, max {max(f):.1f}")
    if not r["problems"]:
        print("- no problems recorded")
    for i, p in enumerate(r["problems"], 1):
        print(f"{i}. **{p['title']}** -- {p['detail']}")
        if p["times"]:
            print(f"   at {', '.join(p['times'])}")
        if p["where"]:
            print(f"   where: {p['where']}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--all", action="store_true", help="every logs/play-*-issues.txt")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = a.files
    if not files:
        found = sorted(glob.glob(os.path.join(root, "logs", "play-*-issues.txt")))
        if not found:
            print("no logs/play-*-issues.txt yet", file=sys.stderr)
            return 1
        files = found if a.all else found[-1:]
    reports = [triage(parse(f)) for f in files]
    if a.json:
        json.dump(reports, sys.stdout, indent=1)
        return 0
    for r in reports:
        print_report(r)
    if len(reports) > 1:
        print("## across sessions")
        seen = Counter(p["title"] for r in reports for p in r["problems"] if p["title"] not in ("log warning", "note"))
        machines = {r["machine"].get("cpu", "?") + " / " + r["machine"].get("gpu", "?") for r in reports}
        print(f"- {len(reports)} sessions on {len(machines)} machine(s)")
        for t, n in seen.most_common(12):
            print(f"- {t}: in {n} of {len(reports)} sessions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
