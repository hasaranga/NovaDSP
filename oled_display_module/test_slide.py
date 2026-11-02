#!/usr/bin/env python3
"""Check sh1106_slide()'s four directions against a per-pixel reference.

The implementation works on bytes (horizontal) and on uint64 columns
(vertical); the reference works one pixel at a time from the definition
"two frames end to end, seen through a sliding window". They must agree
for every shift in the travel.
"""
import random

W, H, PAGES = 128, 64, 8
LEFT, RIGHT, UP, DOWN = 0, 1, 2, 3
NAMES = {LEFT: "LEFT", RIGHT: "RIGHT", UP: "UP", DOWN: "DOWN"}


def travel(d):
    return H if d in (UP, DOWN) else W


def px(buf, x, y):
    return (buf[(y >> 3) * W + x] >> (y & 7)) & 1


def setpx(buf, x, y, v):
    i = (y >> 3) * W + x
    if v:
        buf[i] |= 1 << (y & 7)
    else:
        buf[i] &= ~(1 << (y & 7)) & 0xFF


def reference(prev, inc, s, d):
    """The definition, one pixel at a time."""
    out = bytearray(len(prev))
    for y in range(H):
        for x in range(W):
            if d == LEFT:
                v = px(prev, x + s, y) if x < W - s else px(inc, x - (W - s), y)
            elif d == RIGHT:
                v = px(prev, x - s, y) if x >= s else px(inc, W - s + x, y)
            elif d == UP:
                v = px(prev, x, y + s) if y < H - s else px(inc, x, y - (H - s))
            else:
                v = px(prev, x, y - s) if y >= s else px(inc, x, H - s + y)
            setpx(out, x, y, v)
    return out


def implementation(prev, inc, s, d):
    """What sh1106.c does. fb starts holding the incoming frame."""
    fb = bytearray(inc)

    if d in (LEFT, RIGHT):
        for p in range(PAGES):
            row = bytearray(W)
            base = p * W
            if d == LEFT:
                keep = W - s
                row[0:keep] = prev[base + s:base + W]
                row[keep:W] = fb[base:base + s]
            else:
                row[0:s] = fb[base + W - s:base + W]
                row[s:W] = prev[base:base + W - s]
            fb[base:base + W] = row
    else:
        back = H - s
        for x in range(W):
            i = o = 0
            for p in range(PAGES):
                i |= fb[p * W + x] << (p * 8)
                o |= prev[p * W + x] << (p * 8)
            mask = (1 << 64) - 1
            if d == UP:
                composed = ((o >> s) | (i << back)) & mask
            else:
                composed = ((o << s) | (i >> back)) & mask
            for p in range(PAGES):
                fb[p * W + x] = (composed >> (p * 8)) & 0xFF
    return fb


def main():
    random.seed(20260831)
    bad = 0
    checked = 0

    for trial in range(3):
        prev = bytearray(random.getrandbits(8) for _ in range(W * PAGES))
        inc = bytearray(random.getrandbits(8) for _ in range(W * PAGES))

        for d in (LEFT, RIGHT, UP, DOWN):
            for s in range(1, travel(d)):
                r = reference(prev, inc, s, d)
                a = implementation(prev, inc, s, d)
                checked += 1
                if r != a:
                    bad += 1
                    if bad <= 5:
                        diff = next(k for k in range(len(r)) if r[k] != a[k])
                        print("MISMATCH %-5s shift=%-3d first bad byte %d: "
                              "ref %02x impl %02x"
                              % (NAMES[d], s, diff, r[diff], a[diff]))

    # The ends of the travel, which sh1106_slide() short-circuits.
    print()
    print("checked %d (direction, shift) combinations" % checked)
    print("mismatches: %d" % bad)
    print("PASS" if bad == 0 else "FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
