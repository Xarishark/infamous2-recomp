#!/usr/bin/env python3
"""Resolve HOST_SAMPLE's host_samples.txt against the linker map.

    python tools/host_profile.py [host_samples.txt] [build/infamous2.map] [--top N]

Prints the top functions by inclusive and by self samples (percent of all
samples of the frame-clock thread).
"""
import bisect, re, sys


def load_map(path):
    syms = []
    pref = 0x140000000
    for line in open(path, errors="replace"):
        m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            pref = int(m.group(1), 16)
            continue
        # link.exe: " 0001:00000000  sym  0000000140001000 f  obj"
        m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s", line)
        if m:
            syms.append((int(m.group(2), 16) - pref, m.group(1)))
            continue
        # lld-link: "   Address  Size  Align Out In Symbol" rows; symbol rows have 4 tab/space fields
        m = re.match(r"\s*([0-9a-fA-F]{8,16})\s+[0-9a-fA-F]+\s+\d+\s+(\S.*)$", line)
        if m:
            name = m.group(2).strip()
            if name and not name.startswith((".", "<")) and ":(" not in name and "\\" not in name and "/" not in name:
                syms.append((int(m.group(1), 16), name))
    syms.sort()
    # lld addresses are VAs; normalise to RVAs if they look like VAs
    if syms and syms[len(syms) // 2][0] >= pref:
        syms = [(a - pref, n) for a, n in syms]
    return syms


def thread_names(log_path):
    """host tid -> name, from a run log (UTF-16 or UTF-8)."""
    raw = open(log_path, "rb").read()
    txt = raw.decode("utf-16", errors="replace") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") \
        else raw.replace(b"\0", b"").decode("utf-8", errors="replace")
    guest_name, names = {}, {}
    for m in re.finditer(r"sys_ppu_thread_create tid=(\d+) name=\"([^\"]*)\"", txt):
        guest_name[m.group(1)] = m.group(2)
    for m in re.finditer(r"\[THREAD (\d+)\] host thread started.*?hosttid=(\d+)", txt):
        names[int(m.group(2))] = f"ppu tid {m.group(1)} '{guest_name.get(m.group(1), '?')}'"
    for m in re.finditer(r"\[SPU\] host thread for image (\d+) hosttid=(\d+)", txt):
        names[int(m.group(2))] = f"spu image {m.group(1)}"
    return names


def threads_report(path, syms, log_path, top):
    addrs = [a for a, _ in syms]
    names = thread_names(log_path) if log_path else {}
    blocks, cur = [], None
    for ln in open(path):
        if ln.startswith("thread"):
            p = ln.split()
            cur = {"tid": int(p[1]), "n": int(p[3]), "busy": int(p[5]), "f": {}}
            blocks.append(cur)
        elif cur and ln.strip():
            r, c = ln.split()
            r = int(r, 16)
            k = bisect.bisect_right(addrs, r) - 1
            nm = syms[k][1] if k >= 0 else f"?{r:X}"
            cur["f"][nm] = cur["f"].get(nm, 0) + int(c)
    blocks.sort(key=lambda b: -b["busy"])
    for b in blocks[:top]:
        print(f"host tid {b['tid']:6} {names.get(b['tid'], ''):40} busy {100.0 * b['busy'] / max(b['n'], 1):5.1f}%"
              f" ({b['busy']}/{b['n']})")
        for nm, c in sorted(b["f"].items(), key=lambda x: -x[1])[:5]:
            print(f"        {100.0 * c / max(b['n'], 1):5.1f}%  {nm}")


def main():
    if "--threads" in sys.argv:
        i = sys.argv.index("--threads")
        mp = sys.argv[sys.argv.index("--map") + 1] if "--map" in sys.argv else "build/infamous2.map"
        lg = sys.argv[sys.argv.index("--log") + 1] if "--log" in sys.argv else None
        threads_report(sys.argv[i + 1], load_map(mp), lg, 15)
        return
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    samp = args[0] if args else "host_samples.txt"
    mp = args[1] if len(args) > 1 else "build/infamous2.map"
    syms = load_map(mp)
    addrs = [a for a, _ in syms]
    lines = open(samp).read().split("\n")
    total = int(lines[0].split()[1])
    selfc, incl = {}, {}
    for ln in lines[1:]:
        if not ln.strip():
            continue
        r, s, i = ln.split()
        r = int(r, 16)
        k = bisect.bisect_right(addrs, r) - 1
        name = syms[k][1] if k >= 0 else f"?{r:X}"
        selfc[name] = selfc.get(name, 0) + int(s)
        incl[name] = incl.get(name, 0) + int(i)
    print(f"{total} samples, {len(syms)} map symbols")
    print("\n-- inclusive --")
    for n, c in sorted(incl.items(), key=lambda x: -x[1])[:top]:
        print(f"{100.0 * c / total:6.1f}%  {n}")
    print("\n-- self --")
    for n, c in sorted(selfc.items(), key=lambda x: -x[1])[:top]:
        print(f"{100.0 * c / total:6.1f}%  {n}")


if __name__ == "__main__":
    main()
