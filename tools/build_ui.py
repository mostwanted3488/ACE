#!/usr/bin/env python3
"""Builds ui/index.html (single self-contained file embedded in the plugin)
from ui/src/template.html by inlining the images and fonts as data URIs.

    python3 tools/build_ui.py
"""
import base64
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "ui" / "src"
html = (src / "template.html").read_text(encoding="utf-8")


def data_uri(path: pathlib.Path, mime: str) -> str:
    return f"data:{mime};base64," + base64.b64encode(path.read_bytes()).decode()


assets = {
    "__RIFLE__": "rifle.png",
    "__ICON__": "awp_icon.png",
    "__BULLETPX__": "bullet_px.png",   # must be replaced before __BULLET__
    "__BULLET__": "bullet.png",
    "__CASING__": "casing.png",
}
for key, name in assets.items():
    html = html.replace(key, data_uri(src / "assets" / name, "image/png"))


def font(m: re.Match) -> str:
    return data_uri(src / "fonts" / f"{m.group(1)}.ttf", "font/ttf")


html = re.sub(r"__FONT_([A-Za-z\-]+)__", font, html)

leftover = [p for p in re.findall(r"__[A-Z_]+__", html) if p != "__JUCE__"]
if leftover:
    raise SystemExit(f"unreplaced placeholders: {sorted(set(leftover))}")

out = root / "ui" / "index.html"
out.write_text(html, encoding="utf-8")
print(f"wrote {out.relative_to(root)} ({out.stat().st_size // 1024} KB)")
