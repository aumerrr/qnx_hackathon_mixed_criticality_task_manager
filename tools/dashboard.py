#!/usr/bin/env python3
"""
dashboard.py - QNX core map for the mixed-criticality demo (runs ON the Pi).

Shows, per core: which of our programs run there, PID, TID, priority, policy,
state, CPU use and total CPU time. Flags anything that is not where the plan
says it should be. Read-only: never sends a message to any of our programs.

Sources (only pidin is required; the others are used when available):
  pidin         core (last CPU), runmask, priority, policy, state, CPU time
  taskmgr.out   taskmgr's own printout: per-core load, each registered job's
                CPU share vs budget, and its events (taskmgr must NOT run with -q)
  hogs          CPU % for programs taskmgr does not track (abs, panel, ...)

Run as root, pinned to core 3, low priority:
    su
    on -C3 -p10 python3 /tmp/dashboard.py
Options:
    --tm-log PATH   taskmgr printout (default /tmp/run1/taskmgr.out)
    --interval S    screen refresh, seconds (default 1.0)
    --check         test every data source once, print what it got, exit
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------------------
# THE PLAN (edit here). name: (core, priority, policy)
# name = process name as pidin shows it, without "./"; "dashboard" = this script
# ---------------------------------------------------------------------------
PLAN = {
    "taskmgr":   (0, 50, "FIFO"),
    "abs":       (1, 63, "FIFO"),
<<<<<<< HEAD
    "decoder":   (2, 15, "RR"),
    "nav":       (2, 12, "RR"),
=======
    "airbag":    (1, 62, "FIFO"),
    "decoder":   (2, 15, "RR"),
    "nav":       (2, 12, "RR"),
    "ui":        (2, 10, "RR"),
>>>>>>> 49a13ce (updates)
    "panel":     (3, 10, "RR"),
    "dashboard": (3, 10, "RR"),
}
CORE_NAME = {0: "TASK MANAGER", 1: "SAFETY", 2: "COMFORT POOL", 3: "MONITORING"}
DEMOTE_PRIO = 2                       # taskmgr.c DEMOTE_PRIO
NCORES = 4

# ---------------------------------------------------------------------------
# formats (checked against this Pi's pidin output and taskmgr.c printf calls)
# ---------------------------------------------------------------------------
SEP = "|"
PIDIN_FMT = SEP.join(["%a", "%b", "%p", "%J", "%l", "%z", "%i", "%N", "%h"])
POLICY = {"f": "FIFO", "r": "RR", "o": "OTHER", "s": "SPOR"}

# "[%8u ms] %-12s %-15s pid=%d tid=%d %d -> %d val=%u"
RE_EVENT = re.compile(r"^\[\s*(\d+) ms\]\s+(\S+)\s+(\S+)\s+pid=(-?\d+) tid=(-?\d+) (-?\d+) -> (-?\d+) val=(\d+)")
# "t=%7.1fs %-8s pool %3u%% |" + " C%d %3u%%" per core
RE_STATUS = re.compile(r"^t=\s*([\d.]+)s\s+(\S+)\s+pool\s+(\d+)%\s*\|(.*)$")
RE_CORE = re.compile(r"C(\d+)\s+(\d+)%")
# "    %-15s cls %u prio %3d/%3d share %3u%% budget %3u%% miss %u%s"
RE_TASK = re.compile(r"^\s+(\S+)\s+cls (\d+) prio\s+(-?\d+)/\s*(-?\d+) share\s+(\d+)% budget\s+(\d+)% miss (\d+)(\s+DEMOTED)?")
RE_READY = re.compile(r"^taskmgr: ready \((.*)\)")
# hogs: "PID NAME MSEC PIDS% SYS% MEMORY"
RE_HOGS = re.compile(r"^\s*(\d+)\s+(.+?)\s+(\d+)\s+(\d+)%")
HOGS_SECONDS = 1

HOME, CLEAR, CLEAR_REST = "\033[H", "\033[2J", "\033[J"
HIDE_CUR, SHOW_CUR = "\033[?25l", "\033[?25h"
BOLD, RED, YEL, GRN, DIM, RST = "\033[1m", "\033[31m", "\033[33m", "\033[32m", "\033[2m", "\033[0m"
WIDTH = 79
RE_ANSI = re.compile(r"\033\[[0-9;?]*[A-Za-z]")


def to_int(text, default=None):
    try:
        return int(str(text).strip(), 0)
    except (ValueError, TypeError):
        return default


def bar(pct, width=10):
    pct = max(0.0, min(100.0, pct or 0.0))
    n = int(round(pct / 100.0 * width))
    return "[" + "#" * n + "." * (width - n) + "]"


def visible_len(s):
    return len(RE_ANSI.sub("", s))


def fit(line, width=WIDTH):
    """Cut a line to 'width' visible characters (colour codes don't count) and pad it,
    so every refresh overwrites the previous one exactly and nothing wraps."""
    out, seen, i = [], 0, 0
    while i < len(line) and seen < width:
        m = RE_ANSI.match(line, i)
        if m:
            out.append(m.group(0))
            i = m.end()
            continue
        out.append(line[i])
        seen += 1
        i += 1
    return "".join(out) + RST + " " * (width - seen)


# ---------------------------------------------------------------------------
# pidin
# ---------------------------------------------------------------------------
def run_pidin():
    try:
        out = subprocess.run(["pidin", "-F", PIDIN_FMT], capture_output=True, text=True, timeout=3)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    if out.returncode != 0:
        return None, (out.stderr.strip() or "exit %d" % out.returncode)
    return out.stdout.splitlines(), ""


def mask_to_cores(text):
    mask = to_int(text)
    if mask is None:
        return "?"
    cores = [i for i in range(32) if mask >> i & 1]
    return ",".join(str(c) for c in cores) if cores else "-"


def parse_pidin(lines):
    """One line per thread; with %i, two extra lines follow each thread:
       '\\tRunmask     : 0x00000002' and '\\tInherit Mask: 0x0000000f'."""
    threads, last = [], None
    for line in lines:
        s = line.strip()
        if s.startswith("Runmask"):
            if last is not None:
                last["mask"] = mask_to_cores(s.split(":", 1)[-1])
            continue
        if s.startswith("Inherit"):
            continue
        p = line.split(SEP)
        last = None
        if len(p) < 9:
            continue
        pid, tid = to_int(p[0]), to_int(p[1])
        if pid is None or tid is None:
            continue                                  # header line
        pt = p[2].strip()
        digits = "".join(c for c in pt if c.isdigit())
        last = {"pid": pid, "tid": tid, "prio": to_int(digits, -1),
                "policy": POLICY.get(pt[len(digits):], pt[len(digits):] or "?"),
                "state": p[3].strip(), "core": to_int(p[4], -1), "time": p[5].strip(),
                "mask": "?", "proc": p[7].strip().split("/")[-1]}
        threads.append(last)
    return threads


# ---------------------------------------------------------------------------
# taskmgr.out: reads only new lines each refresh; survives restart/truncation
# ---------------------------------------------------------------------------
class TmLog:
    def __init__(self, path):
        self.path, self.pos, self.partial = path, 0, ""
        self.status = None          # {"t","state","pool","cores":{core: %}}
        self.jobs = {}              # name -> job, from the latest complete status block
        self._block = None
        self.events = []
        self.mode = "?"

    def poll(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return
        if size < self.pos:                      # file recreated -> start over
            self.pos, self.partial = 0, ""
        if size == self.pos:
            return
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read(1 << 20)           # at most 1 MB per refresh
                self.pos = f.tell()
        except OSError:
            return
        lines = (self.partial + data.decode("ascii", errors="replace")).split("\n")
        self.partial = lines.pop()               # keep a half-written last line
        for line in lines:
            self._feed(line.rstrip("\r"))
        if self._block:                          # taskmgr writes a whole block, then fflush:
            self.jobs = dict(self._block)        # show it now, don't wait for the next one

    def _feed(self, line):
        m = RE_STATUS.match(line)
        if m:
            self._end_block()
            self.status = {"t": float(m.group(1)), "state": m.group(2), "pool": int(m.group(3)),
                           "cores": {int(c): int(p) for c, p in RE_CORE.findall(m.group(4))}}
            self._block = {}
            return
        m = RE_TASK.match(line)
        if m and self._block is not None:
            self._block[m.group(1)] = {"cls": int(m.group(2)), "prio": int(m.group(3)),
                                       "base": int(m.group(4)), "share": int(m.group(5)),
                                       "budget": int(m.group(6)), "miss": int(m.group(7)),
                                       "demoted": m.group(8) is not None}
            return
        self._end_block()
        m = RE_EVENT.match(line)
        if m:
            self.events.append({"ms": int(m.group(1)), "kind": m.group(2), "name": m.group(3),
                                "from": int(m.group(6)), "to": int(m.group(7))})
            del self.events[:-50]
            return
        m = RE_READY.match(line)
        if m:
            self.mode = "watch-only" if "off" in m.group(1) else "enforcing"

    def _end_block(self):
        if self._block is not None:
            self.jobs, self._block = self._block, None

    def age(self):
        try:
            return time.time() - os.path.getmtime(self.path)
        except OSError:
            return None


# ---------------------------------------------------------------------------
# hogs, in a background thread: uses the LAST block of "hogs -i 2 -s 1"
# (= the last second). Switches itself off after 3 unusable runs.
# ---------------------------------------------------------------------------
def parse_hogs(text):
    blocks, cur = [], None
    for line in text.splitlines():
        if "PID" in line and "MSEC" in line:     # header = start of one iteration
            cur = {}
            blocks.append(cur)
            continue
        m = RE_HOGS.match(line)
        if m and cur is not None:
            cur[int(m.group(1))] = int(m.group(3)) / (10.0 * HOGS_SECONDS)   # % of one core
    return blocks[-1] if blocks else {}


class Hogs(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.use, self.when, self.status, self.failures = {}, 0.0, "starting", 0

    def run(self):
        while self.failures < 3:
            try:
                out = subprocess.run(["hogs", "-i", "2", "-s", str(HOGS_SECONDS), "-%", "0"],
                                     capture_output=True, text=True, timeout=15)
                block = parse_hogs(out.stdout)
                if block:
                    self.use, self.when, self.status, self.failures = block, time.time(), "ok", 0
                else:
                    self.failures += 1
                    self.status = "hogs output not understood, run --check"
            except (OSError, subprocess.TimeoutExpired) as e:
                self.failures += 1
                self.status = "hogs: %s" % e
                time.sleep(2)
        self.use = {}
        self.status = "hogs off (" + self.status + ")"

    def fresh(self):
        return self.status == "ok" and time.time() - self.when < 3 * HOGS_SECONDS + 3


# ---------------------------------------------------------------------------
# screen
# ---------------------------------------------------------------------------
def build_frame(my_pid, t0, tm, hogs):
    out = []
    lines, err = run_pidin()
    threads = parse_pidin(lines) if lines else []
    for t in threads:
        if t["pid"] == my_pid:
            t["proc"] = "dashboard"
    ours = [t for t in threads if t["proc"] in PLAN]
    running = {t["proc"] for t in ours}
    tm_alive = "taskmgr" in running
    tm_age = tm.age()
    tm_fresh = tm_alive and tm_age is not None and tm_age < 3.0 and tm.status is not None

    # --- title + task manager line ---
    out.append(BOLD + "QNX CORE MAP" + RST + "   %s   up %.0f s" % (
        time.strftime("%H:%M:%S"), time.monotonic() - t0))
    if lines is not None and not tm_alive:
        out.append(BOLD + RED + "TASK MANAGER: NOT RUNNING" + RST)
    elif tm_fresh:
        s = tm.status["state"]
        col = RED if s == "PROTECT" else YEL if s in ("PRESSURE", "RECOVER") else GRN
        out.append(col + "TASK MANAGER: %s   %s   pool %d%%" % (s, tm.mode, tm.status["pool"]) + RST)
    elif tm_alive:
        out.append(YEL + "TASK MANAGER: running, but no fresh lines in --tm-log (-q? wrong path?)" + RST)

    # --- warnings ---
    if os.geteuid() != 0:
        out.append(RED + "NOT ROOT: cores show 0 for root's programs. Run with su." + RST)
    if lines is None:
        out.append(RED + "pidin failed: %s" % err + RST)
    for name in PLAN:
        pids = sorted({t["pid"] for t in ours if t["proc"] == name})
        if len(pids) > 1:
            out.append(YEL + "WARNING: %d copies of %s running (pids %s)" % (
                len(pids), name, " ".join(str(p) for p in pids)) + RST)

    # --- per-core blocks ---
    out.append(DIM + "  %-9s %8s %3s %4s %-4s %-9s %6s %8s  %s" % (
        "PROGRAM", "PID", "TID", "PRIO", "POL", "STATE", "USE%", "CPU TIME", "NOTE") + RST)
    order = list(PLAN)
    for core in range(NCORES):
        load = tm.status["cores"].get(core) if tm_fresh else None
        head = BOLD + "CORE %d  %-13s" % (core, CORE_NAME.get(core, "")) + RST
        if load is not None:
            col = RED if load >= 90 else YEL if load >= 70 else ""
            head += "  load " + col + "%s %3d%%" % (bar(load, 20), load) + RST
        out.append(head)

        here = sorted((t for t in ours if t["core"] == core),
                      key=lambda t: (order.index(t["proc"]), t["pid"], t["tid"]))
        for t in here:
            name = t["proc"]
            want_core, want_prio, want_pol = PLAN[name]
            job = tm.jobs.get(name) if tm_fresh else None
            if job is not None:
                use = float(job["share"])                   # taskmgr's exact measurement
            elif hogs.fresh() and t["pid"] in hogs.use:
                use = hogs.use[t["pid"]]
            else:
                use = None
            notes, col = [], ""
            demoted = job is not None and job["demoted"]
            if demoted:
                notes.append("DEMOTED %d->%d" % (job["base"], DEMOTE_PRIO))
                col = YEL
            elif job is not None and job["budget"] and job["share"] > job["budget"]:
                notes.append("over budget %d%%" % job["budget"])
                col = RED
            if want_core != core:
                notes.append("planned core %d" % want_core)
                col = YEL
            if t["prio"] != want_prio and not demoted:
                notes.append("planned prio %d" % want_prio)
                col = YEL
            if t["policy"] != want_pol:
                notes.append("planned %s" % want_pol)
                col = YEL
            out.append(col + "  %-9s %8d %3d %4d %-4s %-9.9s %6s %8.8s  %s" % (
                name, t["pid"], t["tid"], t["prio"], t["policy"], t["state"],
                "%.1f" % use if use is not None else "-", t["time"], ", ".join(notes)) + RST)

        for name, (want_core, _p, _pol) in PLAN.items():
            if want_core == core and name not in running:
                out.append(DIM + "  %-9s -- not running --" % name + RST)

    # --- footer: last events + where USE% comes from ---
    if tm_fresh and tm.events:
        out.append(DIM + "last task manager events:" + RST)
        for e in tm.events[-3:]:
            out.append(DIM + "  %8.1f s  %-12s %-10s %d -> %d" % (
                e["ms"] / 1000.0, e["kind"], e["name"], e["from"], e["to"]) + RST)
    src = "USE% = % of one core: registered jobs from taskmgr, others from hogs"
    out.append(DIM + src + RST)
    if hogs.status != "ok":
        out.append(DIM + "  [" + hogs.status + "]" + RST)
    out.append(DIM + "Ctrl-C to quit" + RST)
    return out


def live(args):
    my_pid = os.getpid()
    tm = TmLog(args.tm_log)
    hogs = Hogs()
    hogs.start()
    t0 = time.monotonic()
    sys.stdout.write(HIDE_CUR + CLEAR)
    while True:
        start = time.monotonic()
        tm.poll()
        try:
            frame = build_frame(my_pid, t0, tm, hogs)
        except Exception as e:                       # never die on one bad sample
            frame = [RED + "dashboard error: %r (retrying)" % (e,) + RST]
        text = HOME + "\n".join(fit(l) for l in frame) + CLEAR_REST
        sys.stdout.write(text)
        sys.stdout.flush()
        time.sleep(max(0.0, args.interval - (time.monotonic() - start)))


def check(args):
    print("root:", os.geteuid() == 0)
    print("\n== pidin ==")
    lines, err = run_pidin()
    if lines is None:
        print("FAILED:", err)
    else:
        allt = parse_pidin(lines)
        found = [t for t in allt if t["proc"] in PLAN]
        print("%d threads parsed; ours:" % len(allt))
        for t in found:
            print("  ", t)
        if not found:
            print("   (none of %s running)" % ", ".join(PLAN))
    print("\n== taskmgr log: %s ==" % args.tm_log)
    tm = TmLog(args.tm_log)
    tm.poll()
    tm._end_block()
    age = tm.age()
    print("age:", "missing" if age is None else "%.1f s" % age)
    print("mode:", tm.mode)
    print("status:", tm.status)
    print("jobs:", tm.jobs)
    print("events: %d, last: %s" % (len(tm.events), tm.events[-1] if tm.events else None))
    print("\n== hogs -i 2 -s 1 -% 0 ==")
    try:
        out = subprocess.run(["hogs", "-i", "2", "-s", "1", "-%", "0"],
                             capture_output=True, text=True, timeout=15)
        print("\n".join(out.stdout.splitlines()[:12]) or out.stderr.strip())
        print("parsed (pid -> % of one core):", parse_hogs(out.stdout))
    except (OSError, subprocess.TimeoutExpired) as e:
        print("FAILED:", e)


def main():
    ap = argparse.ArgumentParser(description="QNX core map (read-only)")
    ap.add_argument("--tm-log", default="/tmp/run1/taskmgr.out", help="taskmgr printout file")
    ap.add_argument("--interval", type=float, default=1.0, help="refresh period in s (min 0.5)")
    ap.add_argument("--check", action="store_true", help="test each data source once and exit")
    args = ap.parse_args()
    args.interval = max(0.5, args.interval)
    if args.check:
        check(args)
        return 0
    try:
        live(args)
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write(RST + SHOW_CUR + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
