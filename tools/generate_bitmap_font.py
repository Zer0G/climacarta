"""Generate the embedded 1-bpp Meedori Sans fonts used by the e-paper UI."""

from pathlib import Path
from math import ceil

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
REGULAR_SOURCE = ROOT / "font" / "Meedori Sans Typeface" / "Meedori Sans Regular.ttf"
BOLD_SOURCE = ROOT / "font" / "Meedori Sans Typeface" / "Meedori Sans Bold.ttf"
OUTPUT = ROOT / "main" / "font_share_tech_mono.h"
FIRST_CHAR = 32
LAST_CHAR = 122
EXTRA_CHARACTERS = "°"
STATUS_FONT_SIZE = 18
STATUS_HORIZONTAL_SCALE = 0.52
FONT_SIZES = (20, 24, 40, 50)
HORIZONTAL_SCALE = 0.68
SMALL_HORIZONTAL_SCALE = 0.62
MEDIUM_HORIZONTAL_SCALE = 0.64
STATUS_MONO_THRESHOLD = 216
SMALL_MONO_THRESHOLD = 222
MEDIUM_MONO_THRESHOLD = 226
PRIMARY_MONO_THRESHOLD = 184
LARGEST_MONO_THRESHOLD = 181


def render_font(size: int, *, bold: bool = False,
                horizontal_scale: float = HORIZONTAL_SCALE,
                stroke_width: int | None = None,
                threshold: int = PRIMARY_MONO_THRESHOLD):
    font = ImageFont.truetype(str(BOLD_SOURCE if bold else REGULAR_SOURCE), size)
    if stroke_width is None:
        stroke_width = 1 if bold else 0
    characters = [chr(code) for code in range(FIRST_CHAR, LAST_CHAR + 1)] + list(EXTRA_CHARACTERS)
    boxes = [font.getbbox(char, stroke_width=stroke_width)
             for char in characters if char != " "]
    left = min(box[0] for box in boxes)
    top = min(box[1] for box in boxes)
    right = max(box[2] for box in boxes)
    bottom = max(box[3] for box in boxes)
    natural_width = max(ceil(font.getlength(char)) + 2 * stroke_width
                        for char in characters)
    natural_width = max(natural_width, right - left)
    width = max(1, round(natural_width * horizontal_scale))
    height = bottom - top
    stride = (width + 7) // 8
    output = bytearray()

    for char in characters:
        if char == "°":
            # Meedori Sans has no degree glyph. Draw a native bitmap ring near
            # the cap height instead of accepting Pillow's empty fallback.
            image = Image.new("1", (width, height), 0)
            draw = ImageDraw.Draw(image)
            diameter = max(4, round(height * 0.20))
            ring_width = max(1, round(height / 24))
            ring_left = max(0, (width - diameter) // 2)
            draw.ellipse((ring_left, 1, ring_left + diameter - 1, diameter),
                         outline=1, width=ring_width)
        else:
            image = Image.new("L", (natural_width, height), 0)
            draw = ImageDraw.Draw(image)
            draw.text((-left, -top), char, font=font, fill=255,
                      stroke_width=stroke_width, stroke_fill=255)
            image = image.resize((width, height), Image.Resampling.LANCZOS)
            image = image.point(lambda value: 255 if value >= threshold else 0,
                                mode="1")
        for y in range(height):
            for byte_x in range(stride):
                value = 0
                for bit in range(8):
                    x = byte_x * 8 + bit
                    if x < width and image.getpixel((x, y)):
                        value |= 0x80 >> bit
                output.append(value)
    return width, height, stride, output


def format_bytes(data: bytearray) -> list[str]:
    rows = []
    for offset in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16]) + ",")
    return rows


def main() -> None:
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "/* Generated from Meedori Sans Bold by tools/generate_bitmap_font.py. */",
        f"#define SHARE_TECH_MONO_FIRST_CHAR {FIRST_CHAR}",
        f"#define SHARE_TECH_MONO_LAST_CHAR  {LAST_CHAR}",
        f"#define SHARE_TECH_MONO_DEGREE_INDEX {LAST_CHAR - FIRST_CHAR + 1}",
        "",
        "typedef struct {",
        "    uint8_t width;",
        "    uint8_t height;",
        "    uint8_t stride;",
        "    const uint8_t *data;",
        "} bitmap_font_t;",
        "",
    ]

    status_width, status_height, status_stride, status_data = render_font(
        STATUS_FONT_SIZE, bold=True,
        horizontal_scale=STATUS_HORIZONTAL_SCALE,
        stroke_width=1,
        threshold=STATUS_MONO_THRESHOLD,
    )
    lines.append(f"static const uint8_t s_share_tech_mono_status_data[{len(status_data)}] = {{")
    lines.extend(format_bytes(status_data))
    lines.extend([
        "};",
        "",
        "static const bitmap_font_t s_share_tech_mono_status = {",
        f"    .width = {status_width},",
        f"    .height = {status_height},",
        f"    .stride = {status_stride},",
        "    .data = s_share_tech_mono_status_data,",
        "};",
        "",
    ])

    fonts = []
    for index, size in enumerate(FONT_SIZES, start=1):
        width, height, stride, data = render_font(
            size, bold=True, stroke_width=1,
            horizontal_scale=(SMALL_HORIZONTAL_SCALE if index == 1 else
                              MEDIUM_HORIZONTAL_SCALE if index == 2 else
                              HORIZONTAL_SCALE),
            threshold=(SMALL_MONO_THRESHOLD if index == 1 else
                       MEDIUM_MONO_THRESHOLD if index == 2 else
                       PRIMARY_MONO_THRESHOLD if index == 3 else
                       LARGEST_MONO_THRESHOLD),
        )
        name = f"s_share_tech_mono_{size}"
        lines.append(f"static const uint8_t {name}_data[{len(data)}] = {{")
        lines.extend(format_bytes(data))
        lines.extend([
            "};",
            "",
            f"static const bitmap_font_t {name} = {{",
            f"    .width = {width},",
            f"    .height = {height},",
            f"    .stride = {stride},",
            f"    .data = {name}_data,",
            "};",
            "",
        ])
        fonts.append(name)

    lines.extend([
        "static const bitmap_font_t *const s_share_tech_mono_fonts[] = {",
        *(f"    &{name}," for name in fonts),
        "};",
        "",
    ])
    OUTPUT.write_text("\n".join(lines), encoding="ascii")


if __name__ == "__main__":
    main()
