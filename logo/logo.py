#!/usr/bin/env python3
"""logo.py: turn logo/jinxmesh.svg into the image the keyboard view shows while it starts.

    ~/.venv-pw/bin/python -I logo/logo.py [--width 228] [--svg FILE] [--out FILE.c] [--png FILE.png]

The SVG is drawn by headless Chromium (scripts off, no network) at four times the size on a transparent
ground, cut to the drawing, scaled down and written as an LVGL RGB565A8 image: two bytes of color per
pixel, then one plane of alpha. The view draws it on its own ground color, so the logo must be a
transparent one. Needs Playwright with Chromium and Pillow.
"""
import argparse
import tempfile
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright

HERE = Path(__file__).resolve().parent
UI = Path.home() / "github_forks/device-ui"
PAGE = (1280, 1024)  # more than any logo drawn so far needs
SCALE = 4


def render(svg: Path) -> Image.Image:
    with tempfile.TemporaryDirectory() as tmp, sync_playwright() as p:
        shot = Path(tmp) / "logo.png"
        browser = p.chromium.launch()
        ctx = browser.new_context(viewport={"width": PAGE[0], "height": PAGE[1]}, device_scale_factor=SCALE,
                                  java_script_enabled=False, offline=True)
        page = ctx.new_page()
        page.set_content("<style>html,body{margin:0;background:transparent}</style>" + svg.read_text())
        page.screenshot(path=str(shot), omit_background=True,
                        clip={"x": 0, "y": 0, "width": PAGE[0], "height": PAGE[1]})
        browser.close()
        img = Image.open(shot).convert("RGBA")
        img.load()
    box = img.getchannel("A").getbbox()
    if not box:
        raise SystemExit(f"{svg}: nothing drawn")
    return img.crop(box)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--svg", type=Path, default=HERE / "jinxmesh.svg")
    ap.add_argument("--out", type=Path, default=UI / "source/graphics/TFT/KeyboardViewLogo.c")
    ap.add_argument("--png", type=Path, help="also save the scaled image, to look at")
    ap.add_argument("--width", type=int, default=228, help="pixels on the 320x240 screen")
    args = ap.parse_args()

    big = render(args.svg)
    w = args.width
    h = round(big.height * w / big.width)
    # scale with premultiplied alpha, or the edges pick up the color of the invisible pixels
    img = big.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")
    if args.png:
        img.save(args.png)

    color, alpha = bytearray(), bytearray()
    raw = img.tobytes()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) if a else 0
        color += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    data = bytes(color) + bytes(alpha)

    lines = [
        "// Made by logo/logo.py of the harness from logo/jinxmesh.svg: do not edit, run the script again.",
        f"// The JinxMesh logo for the start screen, {w}x{h}, RGB565A8 ({len(data)} bytes).",
        "",
        "#if defined(USE_KEYBOARD_VIEW)",
        "",
        '#include "lvgl.h"',
        "",
        "static const uint8_t kbview_logo_map[] = {",
    ]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
    lines += [
        "};",
        "",
        "const lv_image_dsc_t kbview_logo = {",
        "    .header =",
        "        {",
        "            .magic = LV_IMAGE_HEADER_MAGIC,",
        "            .cf = LV_COLOR_FORMAT_RGB565A8,",
        "            .flags = 0,",
        f"            .w = {w},",
        f"            .h = {h},",
        f"            .stride = {w * 2},",
        "        },",
        "    .data_size = sizeof(kbview_logo_map),",
        "    .data = kbview_logo_map,",
        "};",
        "",
        "#endif",
        "",
    ]
    args.out.write_text("\n".join(lines))
    print(f"{args.out}: {w}x{h}, {len(data)} bytes")


if __name__ == "__main__":
    main()
