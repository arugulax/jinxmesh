#!/usr/bin/env python3
"""monkey.py <keys> [seed] [dive]: random keys into the simulator window; reports whether it survived and how its memory moved."""
import os
import random
import subprocess
import sys
import time

count = int(sys.argv[1])
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
env = {"DISPLAY": os.environ.get("KBVIEW_DISPLAY", ":99"), "PATH": "/usr/bin:/bin"}
NAV = ["Up", "Down", "Left", "Right", "Return", "Escape", "Home", "End", "Delete", "Tab", "Prior", "BackSpace", "ctrl+e", "ctrl+g"]
TEXT = list("abcdefghijklmnopqrstuvwxyz0123456789") + ["space", "at", "period", "minus", "plus", "equal"]


DIVE = len(sys.argv) > 3 and sys.argv[3] == "dive"


def downs(most):
    return ["Down"] * random.randint(0, most)


def typed(most, pool=TEXT):
    return [random.choice(pool) for _ in range(random.randint(0, most))]


def dive():
    """A purposeful walk into one corner of the UI: plain random keys hardly ever get through a picker or an editor."""
    kind = random.choice(["radio", "channel", "channel", "setting", "chat", "node", "map", "wifi", "control", "mapmenu",
                          "message", "search"])
    settings = ["Home", "End", "End", "End", "End"]
    digits = list("0123456789") + ["period"]
    if kind == "radio":
        return settings + ["Return"] + downs(6) + ["Return"] + downs(12) + typed(3, digits) + ["Return", "Return"]
    if kind == "channel":
        return (settings + ["Down", "Return"] + downs(3) + ["Return"] + downs(5) + typed(6) + ["Return"] + downs(4)
                + typed(4) + ["Return"] + downs(2) + ["Return"])
    if kind == "setting":
        return settings + downs(6) + ["Return"] + downs(5) + ["Return"] + downs(8) + typed(5) + ["Return"] + downs(1) + ["Return"]
    if kind == "chat":
        return ["Home", "End"] + downs(6) + ["Return"] + typed(12) + ["Return"] + random.choice([[], ["Delete"] + downs(4) + ["Return"]])
    if kind == "wifi":
        return (settings + ["Down"] * 7 + ["Return"] + downs(6) + ["Return"] + downs(4) + typed(4) + ["Return"] + downs(3)
                + ["Return"] + downs(2) + ["Return"])
    if kind == "control":
        return ["ctrl+e"] + [random.choice(["Down", "Down", "Up", "Left", "Right", "Return"]) for _ in range(14)] + ["Escape"]
    if kind == "mapmenu":
        return (["Home", "End", "End", "End", "Delete"] + downs(6) + ["Return"] + downs(3) + typed(4) + ["Return"] + downs(2)
                + ["Return"] + downs(2) + ["Return"])
    if kind == "message":
        return (["Home", "End"] + downs(4) + ["Return"] + ["Up"] * random.randint(1, 6) + ["Delete"] + downs(2) + ["Return"]
                + downs(7) + ["Return", "Escape"])
    if kind == "search":
        return ["Home", "End"] + typed(3) + ["Return"] + downs(4) + ["Return"] + random.choice([[], ["ctrl+g"] + downs(5) + ["Return"]])
    if kind == "node":
        return ["Home", "End", "End"] + downs(20) + ["Return", "Delete"] + downs(7) + ["Return"] + downs(2) + ["Return"]
    return ["Home", "End", "End", "End"] + [random.choice(["i", "o", "n", "Return", "Up", "Down", "Left", "Right", "Delete"])
                                            for _ in range(20)]


def pid():
    out = subprocess.run(["pgrep", "-x", "meshtasticd"], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else 0


def rss(p):
    for line in open(f"/proc/{p}/status"):
        if line.startswith("VmRSS"):
            return int(line.split()[1])
    return 0


p = pid()
assert p, "simulator is not running"
win = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "."], capture_output=True, text=True, env=env).stdout.split()[0]
start = rss(p)
print(f"pid {p} rss {start} kB", flush=True)
sent = 0
began = time.time()
while sent < count:
    batch = []
    if DIVE and random.random() < 0.5:
        batch = dive()
    else:
        for _ in range(25):
            # navigation weighs more than typing, as on the device
            batch.append(random.choice(NAV) if random.random() < 0.7 else random.choice(TEXT))
    subprocess.run(["xdotool", "key", "--window", win, "--delay", "60"] + batch, env=env, capture_output=True)
    sent += len(batch)
    if pid() != p:
        print(f"DIED after about {sent} keys; last keys: {' '.join(batch)}", flush=True)
        sys.exit(1)
    if sent // 1000 != (sent - len(batch)) // 1000:
        print(f"{sent} keys, rss {rss(p)} kB, {time.time() - began:.0f} s", flush=True)
time.sleep(2)
print(f"survived {sent} keys; rss {start} -> {rss(p)} kB", flush=True)
