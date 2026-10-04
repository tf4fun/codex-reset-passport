#!/usr/bin/env python3
"""Build the licensed, reproducible fixed UI subset with lv_font_conv 1.5.3.

Normal regeneration uses the checked-in subset OTF. When a new glyph is
required, pass --source-font NotoSansCJK-Regular.ttc --font-number 2 (SC face).
See assets/README.md for source, license, versions, and exact commands.
"""
from __future__ import annotations
import argparse
import ast
import hashlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets/fonts"
SOURCE_FILES = (ROOT / "main/reset_ui.c", ROOT / "main/reset_presenter.c")
TEXT_INVENTORY = FONT_DIR / "reset_font_text.txt"
FONT_SOURCE = FONT_DIR / "passport_reset_source.otf"
FONT_OUTPUT = FONT_DIR / "reset_font_16.c"
FONT_SIZES = (12, 16)
def hero_inventory() -> str:
    """Derive the tiny 24px font from the relative-age formatter, not guesses."""
    source = (ROOT / "main/reset_presenter.c").read_text(encoding="utf-8")
    start = source.index("static void relative(")
    source = source[start:source.index("\n}\n", start) + 3]
    chars = set("0123456789-")
    for token in re.findall(r'"(?:\\.|[^"\\])*"', source):
        chars.update(re.sub(r"%lld", "", ast.literal_eval(token)))
    return "".join(sorted(chars))


def inventory() -> list[str]:
    result: set[str] = set()
    for path in SOURCE_FILES:
        source = path.read_text(encoding="utf-8")
        for token in re.findall(r'"(?:\\.|[^"\\])*"', source):
            value = ast.literal_eval(token)
            if any(ord(c) > 127 for c in value):
                result.add(value)
    return sorted(result)


def generated_coverage(source: str) -> set[int]:
    """Read converter 1.5.3's tiny cmaps without needing fonttools or LVGL."""
    arrays = {
        name: [int(value, 0) for value in re.findall(r"0x[0-9a-fA-F]+|[0-9]+", body)]
        for name, body in re.findall(
            r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", source, re.S)
    }
    matched = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\] =\s*\{(.*?)\n\};",
                        source, re.S)
    if not matched:
        raise ValueError("Generated C font has no recognized cmap table")
    glyph_count = len(re.findall(r"\.bitmap_index\s*=", source))
    coverage: set[int] = set()
    entries = re.findall(r"\{([^{}]+)\}", matched.group(1))
    if not entries:
        raise ValueError("Generated C font has an empty cmap table")
    for entry in entries:
        fields = dict(re.findall(r"\.(\w+)\s*=\s*(\w+)", entry))
        start = int(fields["range_start"])
        length = int(fields["range_length"])
        first = int(fields["glyph_id_start"])
        if fields["type"] == "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
            offsets = list(range(length))
        elif fields["type"] == "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":
            offsets = arrays[fields["unicode_list"]]
            if len(offsets) != int(fields["list_length"]):
                raise ValueError("Sparse cmap length disagrees with its Unicode list")
        else:
            raise ValueError("Unexpected cmap format: inspect/update the checker")
        if (offsets != sorted(set(offsets)) or not offsets or offsets[-1] >= length
                or first < 1 or first + len(offsets) > glyph_count):
            raise ValueError("Invalid cmap offsets or glyph descriptor bounds")
        coverage.update(start + offset for offset in offsets)
    return coverage


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", default="lv_font_conv")
    parser.add_argument("--source-font", type=Path)
    parser.add_argument("--font-number", type=int, default=2)
    parser.add_argument("--check", action="store_true",
                        help="Check source/inventory coverage without rewriting files")
    args = parser.parse_args()
    hero_text = hero_inventory()
    lines = inventory()
    text = "\n".join(lines) + "\n"
    codepoints = set(range(0x20, 0x7F)) | {ord(c) for c in text if ord(c) >= 0x20} | set(map(ord, hero_text))
    if args.check:
        if TEXT_INVENTORY.read_text(encoding="utf-8") != text:
            raise SystemExit("UI inventory changed: regenerate the font")
        try:
            coverage = set.intersection(*(generated_coverage((FONT_DIR / f"reset_font_{size}.c").read_text(encoding="utf-8")) for size in FONT_SIZES))
        except (ValueError, KeyError) as error:
            raise SystemExit(f"Invalid generated font: {error}") from error
        missing = sorted(codepoints - coverage)
        if missing:
            raise SystemExit("Generated font is missing: " + ", ".join(f"U+{c:04X}" for c in missing))
        if 0x9F98 in coverage:
            raise SystemExit("Known-missing U+9F98 unexpectedly appears in generated font")
        hero_coverage = generated_coverage((FONT_DIR / "reset_font_24.c").read_text(encoding="utf-8"))
        if set(map(ord, hero_text)) - hero_coverage:
            raise SystemExit("Hero font coverage failed")
        print(f"PASS: {len(lines)} fixed non-ASCII strings, {len(codepoints)} glyphs covered; limited 24px hero subset checked")
        return
    from fontTools.ttLib import TTFont
    source = args.source_font or FONT_SOURCE
    font = TTFont(str(source), fontNumber=args.font_number if source.suffix == ".ttc" else -1,
                  recalcTimestamp=False)
    missing = sorted(codepoints - set(font.getBestCmap()))
    if missing:
        raise SystemExit("Source font is missing: " + ", ".join(f"U+{c:04X}" for c in missing))
    if args.source_font:
        from fontTools import subset
        options = subset.Options()
        options.recalc_timestamp = False
        options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
        options.name_legacy = True
        options.name_languages = [0x409]
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(unicodes=codepoints)
        subsetter.subset(font)
        # Give this modified subset its own family name, preserve copyright/OFL.
        for name_id, value in {
            1: "Passport Reset UI", 2: "Regular", 3: "PassportResetUI-Regular-2.004",
            4: "Passport Reset UI Regular", 6: "PassportResetUI-Regular"
        }.items():
            font["name"].setName(value, name_id, 3, 1, 0x409)
        if "CFF " in font:
            cff = font["CFF "].cff
            cff.fontNames = ["PassportResetUI-Regular"]
            cff.topDictIndex[0].FamilyName = "Passport Reset UI"
            cff.topDictIndex[0].FullName = "Passport Reset UI Regular"
        font.save(FONT_SOURCE)
    TEXT_INVENTORY.write_text(text, encoding="utf-8")
    version = subprocess.check_output([args.converter, "--version"], text=True).strip()
    if version != "1.5.3":
        raise SystemExit(f"Expected lv_font_conv 1.5.3; got {version}")
    symbols = "".join(chr(c) for c in sorted(codepoints) if c >= 0x7f)
    for size in (*FONT_SIZES, 24):
        output = FONT_DIR / f"reset_font_{size}.c"
        command = [args.converter, "--font", str(FONT_SOURCE.relative_to(ROOT)),
                   "--symbols=" + (hero_text if size == 24 else "".join(chr(c) for c in sorted(codepoints))),
                   "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
                   "--lv-font-name", f"reset_font_{size}", "--lv-include", "lvgl.h",
                   "--output", str(output.relative_to(ROOT))]
        subprocess.run(command, cwd=ROOT, check=True)
        body = output.read_text(encoding="utf-8")
        output.write_text("/* Copyright 2014-2021 Adobe. Font asset licensed under SIL OFL 1.1.\n"
                               " * Source: Noto Sans CJK SC 2.004. See OFL.txt and assets/README.md. */\n" + body,
                               encoding="utf-8")
        print(f"Generated {len(hero_text) if size == 24 else len(codepoints)} glyphs: {output.relative_to(ROOT)}")
    print("Source subset SHA-256:", hashlib.sha256(FONT_SOURCE.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
