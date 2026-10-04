<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

### Codex reset observer UI subset

- `fonts/reset_font_12.c` / `fonts/reset_font_16.c` are the firmware's `reset_font_12` / `reset_font_16`: 12/16 px, 4 bpp,
  uncompressed LVGL bitmap font, including printable ASCII and every fixed
  non-ASCII string in `main/reset_ui.c` and `main/reset_presenter.c`.
- `fonts/reset_font_24.c` is a 25-glyph, 24px relative-time-only subset, derived
  from the presenter formatter. It does not add a full-size CJK font.
- `fonts/passport_reset_source.otf` is the matching reusable, renamed source
  subset, derived from Noto Sans CJK SC Regular 2.004, TTC face 2. Upstream:
  [Noto CJK](https://github.com/notofonts/noto-cjk). The source system TTC was
  `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`, SHA-256
  `b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a`.
- All font assets remain under the [SIL Open Font License 1.1](fonts/OFL.txt).
  Copyright 2014–2021 Adobe; upstream packaging also credits 2010–2012 Google.
  The source subset uses the new family name **Passport Reset UI**.
- `fonts/reset_font_text.txt` records the actual string inventory. This font
  does not support arbitrary server strings, SSIDs, or personal names; the
  application checks each original announcement/forecast codepoint against the
  active font, substitutes unsupported glyphs with `?`, and visibly flags it.
- `main/CMakeLists.txt` compiles the C asset once. Chinese labels explicitly
  select `reset_font_12`, `reset_font_16`, or the relative-time-only
  `reset_font_24`; ASCII-only sizes use enabled Montserrat fonts.

Reproduction uses Python `fonttools==4.61.1` and official `lv_font_conv@1.5.3`.
The binary source subset is included, so normal regeneration needs no upstream
font download. Install tools into an isolated environment, then run:

```bash
python3 -m pip install fonttools==4.61.1
npm install --prefix /tmp/passport-font-tools --no-audit --no-fund lv_font_conv@1.5.3
python3 tools/generate_reset_font.py \
  --converter /tmp/passport-font-tools/node_modules/.bin/lv_font_conv
python3 tools/generate_reset_font.py --check
```

For new characters absent from the source subset, provide the licensed original:

```bash
python3 tools/generate_reset_font.py \
  --source-font /path/to/NotoSansCJK-Regular.ttc --font-number 2 \
  --converter /tmp/passport-font-tools/node_modules/.bin/lv_font_conv
```

The generator derives the inventory from source, checks source-font coverage,
renames the subset family, and invokes sizes 12/16 plus the tiny 24px subset,
all at bpp 4 / `--no-compress` with
stable relative paths. Review the inventory and generated changes together.

The actual LVGL host preview requires a C/C++ compiler, CMake, Python Pillow,
and resolved LVGL managed sources. It compiles `reset_ui.c`, the presenter,
and the bounded feed parser unchanged. Run:

```bash
python3 tools/render_reset_preview.py
# Optional locally captured response from the read-only status API:
python3 tools/render_reset_preview.py --feed /path/to/status.json
```

PNG files, a labeled synthetic-state contact sheet, glyph/clipping audit, and
source-hash manifest are written under `build/reset-ui-preview/screens/`.
The optional captured-feed image uses the response's generation time and an
unavailable battery placeholder. It is a host rendering, not a physical device
screenshot. The harness checks every font-inventory glyph, verifies that the
known-missing U+9F98 is rejected, audits the active font on each rendered label,
and rejects clipped labels. Device rendering and internal heap remain separate
hardware checks; neither this preview nor compilation establishes them.

The v2.3 announcement/reader regeneration covers 123 fixed strings and 324
codepoints in both 12/16px assets. The 24px font contains only 25 codepoints
needed by the relative-age formatter. Source subset SHA-256:
`fe18e600a2d7fa2373387f8fe32e70f088bcfc06021f94b416636544503f8cf4`.
The host's font objects contain 23734/36322/5056 bytes of text/read-only data
for 12/16/24px respectively (host object measurements, not final firmware sizes).
The actual-render tool emits the three production-state-machine hold GIFs,
original-text and all-page limit fixtures, and an unlabeled fill-stage strip;
see [the preview details](../tools/reset_ui_preview/README.md).
