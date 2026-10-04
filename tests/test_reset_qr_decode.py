#!/usr/bin/env python3
"""Independently decode actual LVGL screen PNGs using ZXing-C++ (not encoder)."""
import argparse
import json
from pathlib import Path
from PIL import Image
import zxingcpp

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('screens', type=Path)
parser.add_argument('--feed', type=Path)
args = parser.parse_args()
site = 'https://codex-resets.com/zh-CN'
cases = {
    '46-source-fallback.png': site,
    '47-source-max-bound.png': 'https://x.com/test/status/' + '7' * (120 - len('https://x.com/test/status/')),
    '48-source-unterminated-fallback.png': site,
    '49-source-observed-fallback.png': site,
    '53-frozen-reader-source-qr.png': 'https://x.com/test/status/123',
}
if args.feed:
    feed = json.loads(args.feed.read_text())['data']
    # The large card selects an explicit schedule before the latest event,
    # independently of relative announcement publication times.
    selected = feed.get('scheduled_reset') or feed.get('latest_reset')
    expected = selected['source']['url'] if selected and selected['source']['type'] != 'observed' else site
    candidates = sorted(args.screens.glob('00-captured-reader-*.png'), key=lambda p: int(p.stem.split('-')[-1]))
    assert candidates
    cases[candidates[-1].name] = expected
for name, expected in cases.items():
    im = Image.open(args.screens / name).convert('RGB')
    assert im.size == (240, 320)
    decoded = zxingcpp.read_barcodes(im)
    assert len(decoded) == 1 and decoded[0].text == expected, (name, decoded, expected)
    # Verify full white margin and integral >=3px modules from actual pixels.
    crop = im.crop((24, 82, 216, 274))
    black = [(x, y) for y in range(192) for x in range(192) if crop.getpixel((x, y)) == (0, 0, 0)]
    min_x, max_x = min(x for x,y in black), max(x for x,y in black)
    min_y, max_y = min(y for x,y in black), max(y for x,y in black)
    # Top-left finder is exactly seven modules wide.
    x = min_x
    while crop.getpixel((x, min_y)) == (0, 0, 0): x += 1
    assert (x - min_x) % 7 == 0
    scale = (x - min_x) // 7
    assert scale >= 3
    assert min(min_x, min_y, 191-max_x, 191-max_y) >= 4*scale
    for y in range(192):
        for x in range(192):
            if x < min_x or y < min_y or x > max_x or y > max_y:
                assert crop.getpixel((x,y)) == (255,255,255)
    print(f'PASS {name}: exact URL, {scale}px modules, >=4-module quiet zone')
