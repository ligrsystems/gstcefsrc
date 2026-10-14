#!/usr/bin/env python3
"""Check cefsrc alpha against html/alpha_test.html.

Usage: check_alpha_frames.py RAW_BGRA COMPOSITED_BGRA WIDTH HEIGHT GREY

Both files are raw frame dumps from filesink; the last frame of each is checked
so the page has finished loading. RAW_BGRA comes straight from cefsrc.
GStreamer BGRA caps mean straight alpha, so each stripe must carry its CSS
colour unchanged. COMPOSITED_BGRA is the same
frame through the GStreamer compositor over a solid GREY background, so each
stripe must equal colour * a + grey * (1 - a). Premultiplied output fails both:
a 50 % white stripe reads 128 instead of 255 raw, and 128 instead of 192
composited.
"""
import sys

# (CSS colour as R, G, B, CSS alpha), one per quarter of the width.
STRIPES = [
    ((255, 255, 255), 0.25),
    ((255, 255, 255), 0.5),
    ((255, 255, 255), 0.75),
    ((255, 128, 0), 0.5),
]
TOLERANCE = 3


def pixel(frame, width, x, y):
    i = (y * width + x) * 4
    b, g, r, a = frame[i : i + 4]
    return r, g, b, a


def check(raw, comp, width, height, grey):
    failures = []
    y = height // 2
    for n, (rgb, css_alpha) in enumerate(STRIPES):
        x = width * (2 * n + 1) // 8
        a8 = round(css_alpha * 255)
        want_raw = (*rgb, a8)
        got_raw = pixel(raw, width, x, y)
        a = a8 / 255
        want_comp = tuple(round(c * a + grey * (1 - a)) for c in rgb)
        got_comp = pixel(comp, width, x, y)[:3]
        print(
            f"stripe {n} rgba({rgb[0]},{rgb[1]},{rgb[2]},{css_alpha}): "
            f"raw RGBA {got_raw} want {want_raw}; "
            f"over grey {grey} RGB {got_comp} want {want_comp}"
        )
        if any(abs(g - w) > TOLERANCE for g, w in zip(got_raw, want_raw)):
            failures.append(f"stripe {n} raw {got_raw} != {want_raw}")
        if any(abs(g - w) > TOLERANCE for g, w in zip(got_comp, want_comp)):
            failures.append(f"stripe {n} composited {got_comp} != {want_comp}")
    return failures


def main(argv):
    raw_path, comp_path, width, height, grey = argv[1:6]
    width, height, grey = int(width), int(height), int(grey)
    size = width * height * 4
    frames = []
    for path in (raw_path, comp_path):
        with open(path, "rb") as f:
            data = f.read()
        if not data or len(data) % size:
            print(f"{path}: {len(data)} bytes is not a whole number of {size}-byte frames")
            return 1
        frames.append(data[-size:])
    raw, comp = frames
    failures = check(raw, comp, width, height, grey)
    for failure in failures:
        print(f"FAIL: {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
