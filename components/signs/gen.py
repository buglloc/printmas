#!/usr/bin/env python
from __future__ import annotations

import os
import re
import random
import argparse
from dataclasses import dataclass
from typing import List, Tuple

from PIL import Image, ImageDraw, ImageFont

# ================= CONFIG =================
WIDTH = 384
HEIGHT = 240

TEXT_PADDING = 28
TEXT_SAFE_PADDING = 20
TEXT_VERTICAL_MARGIN = 40

GLYPH_PADDING = 16
GLYPH_COUNT = (4, 16)
GLYPH_FONT_SIZES = (16, 28)

FONT_DIR = "fonts"
FONT_NAME = "DejaVuSans-Bold.ttf"
GLYPHS = ["★", "✶", "✷", "+", "x", "@", "#"]

THRESHOLD = 140

# ================= UTIL =================
def load_font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(os.path.join(FONT_DIR, FONT_NAME), size)

def rects_intersect(a, b, pad=0) -> bool:
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    return not (
        ax2 + pad < bx1 or
        ax1 > bx2 + pad or
        ay2 + pad < by1 or
        ay1 > by2 + pad
    )

# ================= TEXT LAYOUT =================
def split_by_punctuation(text: str) -> List[str]:
    parts = re.split(r'(?<=[\.\,\!\?\:\;\—\–\-\/])\s+', text)
    return [p.strip() for p in parts if p.strip()]

def pack_sentences(parts: List[str]) -> List[str]:
    if len(parts) <= 2:
        return parts
    return [parts[0], " ".join(parts[1:])]

def split_text_by_width(draw, text, font, max_w):
    chunks = split_by_punctuation(text)
    if len(chunks) > 1:
        left = []
        for c in chunks:
            test = " ".join(left + [c])
            if draw.textlength(test, font=font) <= max_w:
                left.append(c)
            else:
                break
        right = chunks[len(left):]
        if left and right:
            return [" ".join(left), " ".join(right)]

    words = text.split()
    left = []
    for w in words:
        left.append(w)
        if draw.textlength(" ".join(left), font=font) > max_w:
            left.pop()
            break
    return [" ".join(left), " ".join(words[len(left):])]

def best_font_and_lines(text: str):
    img = Image.new("L", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(img)

    max_w = WIDTH - TEXT_PADDING * 2
    max_h = HEIGHT - TEXT_PADDING * 2 - TEXT_VERTICAL_MARGIN * 2
    parts = split_by_punctuation(text)

    for size in range(44, 18, -1):
        font = load_font(size)
        lh = font.getbbox("Hg")[3] + 4

        if draw.textlength(text, font=font) <= max_w and lh <= max_h:
            return font, [text]

        lines = pack_sentences(parts)
        if lh * len(lines) <= max_h and max(draw.textlength(l, font=font) for l in lines) <= max_w:
            return font, lines

        if len(parts) == 1:
            split = split_text_by_width(draw, text, font, max_w)
            if lh * 2 <= max_h and max(draw.textlength(l, font=font) for l in split) <= max_w:
                return font, split

    font = load_font(22)
    return font, split_text_by_width(draw, text, font, max_w)

def draw_text(draw, lines, font):
    lh = font.getbbox("Hg")[3] + 4
    total_h = lh * len(lines)
    y = TEXT_VERTICAL_MARGIN + (HEIGHT - TEXT_VERTICAL_MARGIN * 2 - total_h) // 2

    max_w = max(draw.textlength(l, font=font) for l in lines)
    x1 = (WIDTH - max_w) // 2
    rect = (x1, y, x1 + max_w, y + total_h)

    for line in lines:
        w = draw.textlength(line, font=font)
        x = (WIDTH - w) // 2
        draw.text((x, y), line, font=font, fill=0)
        y += lh

    return rect

def draw_glyphs(draw, forbid_rect):
    boxes = []
    target = random.randint(*GLYPH_COUNT)

    while len(boxes) < target:
        g = random.choice(GLYPHS)
        size = random.randint(*GLYPH_FONT_SIZES)
        font = load_font(size)

        bx = font.getbbox(g)
        gw, gh = bx[2] - bx[0], bx[3] - bx[1]

        x = random.randint(0, WIDTH - gw)
        y = random.randint(0, HEIGHT - gh)
        rect = (x, y, x + gw, y + gh)

        if rects_intersect(rect, forbid_rect, GLYPH_PADDING):
            continue
        if any(rects_intersect(rect, r, GLYPH_PADDING) for r in boxes):
            continue

        draw.text((x, y), g, font=font, fill=0)
        boxes.append(rect)

# ================= RLE =================
@dataclass
class RleImage:
    w: int
    h: int
    row_offs: List[int]
    data: bytes

def img_to_rle(img: Image.Image) -> RleImage:
    img = img.convert("1")
    px = img.load()
    w, h = img.size

    row_offs = []
    out = bytearray()

    def emit(v, n):
        while n:
            k = min(n, 127)
            out.append(((v & 1) << 7) | k)
            n -= k

    for y in range(h):
        row_offs.append(len(out))
        cur = 1 if px[0, y] == 0 else 0
        run = 1
        for x in range(1, w):
            b = 1 if px[x, y] == 0 else 0
            if b == cur:
                run += 1
            else:
                emit(cur, run)
                cur = b
                run = 1
        emit(cur, run)

    return RleImage(w, h, row_offs, bytes(out))

# ================= CODEGEN =================
def _hex_array(data: bytes, items_per_line: int = 16) -> str:
    """Format byte array as hex values with line breaks for readability."""
    lines = []
    for i in range(0, len(data), items_per_line):
        chunk = data[i:i + items_per_line]
        lines.append(", ".join(f"0x{b:02X}" for b in chunk))
    return ",\n  ".join(lines)


def _int_array(values: List[int], items_per_line: int = 12) -> str:
    """Format int array with line breaks for readability."""
    lines = []
    for i in range(0, len(values), items_per_line):
        chunk = values[i:i + items_per_line]
        lines.append(", ".join(str(v) for v in chunk))
    return ",\n  ".join(lines)


def write_cpp(images: List[RleImage], hpp: str, cpp: str):
    n = len(images)

    with open(hpp, "w", encoding="utf-8") as h:
        h.write("""\
#pragma once

#include <cstdint>

namespace PRNM::Signs {

struct RleImage {
  uint16_t w, h;
  const uint32_t* row_offs;
  const uint8_t* data;
};

void Initialize();
const RleImage* Next();

void decode_rle_row_1bpp(
  const RleImage& img,
  uint16_t y,
  uint8_t* row_data,
  uint16_t row_bytes);

}
""")

    with open(cpp, "w", encoding="utf-8") as c:
        c.write(f"""\
#include "signs.h"

#include <cstring>

#include <esp_random.h>

namespace PRNM::Signs {{

namespace {{

constexpr size_t kNumSigns = {n};

size_t indices_[kNumSigns] = {{0}};
size_t current_idx_ = 0;
bool initialized_ = false;

void Shuffle() {{
  for (size_t i = kNumSigns - 1; i > 0; --i) {{
    size_t j = esp_random() % (i + 1);
    size_t tmp = indices_[i];
    indices_[i] = indices_[j];
    indices_[j] = tmp;
  }}
  current_idx_ = 0;
}}

""")

        # Write image data
        for i, img in enumerate(images):
            c.write(f"constexpr uint32_t kRowOffs{i}[] = {{\n  {_int_array(img.row_offs)}\n}};\n\n")
            c.write(f"constexpr uint8_t kRowData{i}[] = {{\n  {_hex_array(img.data)}\n}};\n\n")
            c.write(f"constexpr RleImage kSign{i} = {{{img.w}, {img.h}, kRowOffs{i}, kRowData{i}}};\n\n")

        # Write table
        c.write("constexpr const RleImage* kTable[] = {\n")
        for i in range(n):
            c.write(f"  &kSign{i},\n")
        c.write("};\n\n")

        c.write(f"""\
}}

void Initialize() {{
  for (size_t i = 0; i < kNumSigns; ++i) {{
    indices_[i] = i;
  }}
  Shuffle();
  initialized_ = true;
}}

const RleImage* Next() {{
  if (!initialized_) {{
    Initialize();
  }}

  size_t idx = indices_[current_idx_++];
  if (current_idx_ >= kNumSigns) {{
    Shuffle();
  }}

  return kTable[idx];
}}

void decode_rle_row_1bpp(
    const RleImage& img,
    uint16_t y,
    uint8_t* row_data,
    uint16_t row_bytes) {{
  memset(row_data, 0x00, row_bytes);
  if (y >= img.h) {{
    return;
  }}

  const uint8_t* p = img.data + img.row_offs[y];
  uint16_t x = 0;

  while (x < img.w) {{
    uint8_t t = *p++;
    bool black = (t & 0x80) != 0;
    uint8_t run = t & 0x7F;

    if (black) {{
      for (uint8_t i = 0; i < run && x < img.w; ++i, ++x) {{
        row_data[x >> 3] |= (0x80 >> (x & 7));
      }}
    }} else {{
      x += run;
    }}
  }}
}}

}}
""")

# ================= MAIN =================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quotes", default="quotes.txt")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--hpp", default="signs.h")
    ap.add_argument("--cpp", default="signs.cc")
    ap.add_argument("--emit-png", action="store_true",
                help="also save rendered labels as PNG for preview")
    ap.add_argument("--png-out", default="out",
                    help="directory for preview PNGs")
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    if args.emit_png:
        os.makedirs(args.png_out, exist_ok=True)

    images = []
    png_index = 1

    with open(args.quotes, encoding="utf-8") as f:
        for line in f:
            text = line.strip()
            if not text:
                continue

            img = Image.new("L", (WIDTH, HEIGHT), 255)
            draw = ImageDraw.Draw(img)

            font, lines = best_font_and_lines(text)
            rect = draw_text(draw, lines, font)

            safe = (
                max(0, rect[0] - TEXT_SAFE_PADDING),
                max(0, rect[1] - TEXT_SAFE_PADDING),
                min(WIDTH, rect[2] + TEXT_SAFE_PADDING),
                min(HEIGHT, rect[3] + TEXT_SAFE_PADDING),
            )

            draw_glyphs(draw, safe)

            bw = img.point(lambda p: 0 if p < THRESHOLD else 255, mode="1")

            # --- SAVE PNG FOR PREVIEW ---
            if args.emit_png:
                path = os.path.join(args.png_out, f"{png_index}.png")
                bw.save(path)
                png_index += 1

            images.append(img_to_rle(bw))

    write_cpp(images, args.hpp, args.cpp)
    print(f"Generated {args.hpp} and {args.cpp}")

    if args.emit_png:
        print(f"Preview PNGs saved to: {args.png_out}/")

if __name__ == "__main__":
    main()
