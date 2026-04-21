#!/usr/bin/env python3

from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont


def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
    ]
    for candidate in candidates:
        path = Path(candidate)
        if path.exists():
            return ImageFont.truetype(str(path), size=size)
    return ImageFont.load_default()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: render_terminal_png.py <input.txt> <output.png>", file=sys.stderr)
        return 1

    text = Path(sys.argv[1]).read_text(encoding="utf-8")
    lines = text.splitlines() or [""]

    font = load_font(22)
    padding_x = 24
    padding_y = 24
    line_gap = 10
    header_h = 44

    dummy = Image.new("RGB", (10, 10))
    draw = ImageDraw.Draw(dummy)
    line_height = draw.textbbox((0, 0), "Ag", font=font)[3] + line_gap
    max_width = max(draw.textbbox((0, 0), line, font=font)[2] for line in lines)

    width = max(960, max_width + padding_x * 2)
    height = header_h + padding_y * 2 + line_height * len(lines)

    image = Image.new("RGB", (width, height), "#0d1117")
    draw = ImageDraw.Draw(image)

    draw.rounded_rectangle((0, 0, width - 1, height - 1), radius=18, fill="#0d1117", outline="#30363d")
    draw.rounded_rectangle((0, 0, width - 1, header_h), radius=18, fill="#161b22", outline="#30363d")

    for i, color in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        x = 24 + i * 22
        draw.ellipse((x, 14, x + 12, 26), fill=color)

    y = header_h + padding_y
    for line in lines:
        draw.text((padding_x, y), line, font=font, fill="#c9d1d9")
        y += line_height

    Path(sys.argv[2]).parent.mkdir(parents=True, exist_ok=True)
    image.save(sys.argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
