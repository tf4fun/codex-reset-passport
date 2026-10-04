#!/usr/bin/env python3
"""Build/render the firmware LVGL UI on the host, auditing glyphs and clipping."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def hold_artifacts(output: Path, frames: list[Image.Image]) -> None:
    """Export only actual LCD pixels; renderer provenance lives in the manifest."""
    if frames:
        # GIF stores repeated static frames as a single longer frame if optimized.
        frames[0].save(output / "hold-to-sleep-actual-lvgl.gif", save_all=True,
                       append_images=frames[1:], duration=40, loop=0,
                       disposal=2, optimize=False)
    snapshots = ["18-hold-000-percent.png", "18-hold-025-percent.png",
                 "18-hold-050-percent.png", "18-hold-100-percent.png"]
    strip = Image.new("RGB", (1008, 352), "#26201A")
    for index, name in enumerate(snapshots):
        with Image.open(output / name) as screen:
            strip.paste(screen, (12 + index * 248, 16))
    strip.save(output / "hold-to-sleep-progress-strip.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feed", type=Path, help="Optional captured API JSON; parsed by firmware parser")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/reset-ui-preview")
    parser.add_argument("--output", type=Path, default=ROOT / "build/reset-ui-preview/screens")
    args = parser.parse_args()
    cmake = shutil.which("cmake")
    if not cmake:
        raise SystemExit("cmake is not on PATH. Activate the ESP-IDF or host build environment.")
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([cmake, "-S", str(ROOT / "tools/reset_ui_preview"), "-B", str(build),
                    "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run([cmake, "--build", str(build), "--parallel", "8"], check=True)
    command = [str(build / "reset_ui_preview"), str(output)]
    if args.feed:
        command.append(str(args.feed.resolve()))
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    (output / "audit.txt").write_text(result.stdout, encoding="utf-8")
    images, hold_frames = [], []
    for ppm in sorted(output.glob("*.ppm")):
        with Image.open(ppm) as source:
            image = source.convert("RGB")
        image.save(ppm.with_suffix(".png"))
        if ppm.stem.startswith("hold-frame-"):
            hold_frames.append(image)
        elif not ppm.stem.startswith("00-"):
            images.append((ppm.stem, image))
    hold_artifacts(output, hold_frames)
    if images:
        cols, cell_w, cell_h = 3, 256, 356
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * cell_w + 16, rows * cell_h + 66), "#031216")
        draw = ImageDraw.Draw(sheet)
        font = ImageFont.load_default(size=13)
        draw.text((16, 13), "AI Passport | Actual LVGL 9.5 software rendering", fill="#E6F2EB", font=font)
        draw.text((16, 33), "Synthetic state fixtures. 240 x 320 px per screen. Hardware unverified.",
                  fill="#8CAAA7", font=font)
        for i, (name, image) in enumerate(images):
            x, y = 16 + i % cols * cell_w, 66 + i // cols * cell_h
            draw.text((x, y), name, fill="#A8E8C6", font=font)
            sheet.paste(image, (x, y + 23))
        sheet.save(output / "synthetic-states-contact-sheet.png")
    captured = json.loads(args.feed.read_text(encoding="utf-8")) if args.feed else {}
    manifest = {
        "renderer": "Actual main/reset_ui.c + reset_presenter.c; LVGL 9.5 software RGB565",
        "dimensions": [240, 320],
        "rounding": "Actual BSP rounded-row geometry, black outside visible span",
        "radius": int(re.search(r"#define\s+BSP_LVGL_SCREEN_RADIUS\s+(\d+)",
                      (ROOT / "components/bsp/include/bsp_display.h").read_text()).group(1)), "hardware_validation": False,
        "scenarios": "Synthetic unless named 00-captured-feed",
        "captured_feed_sha256": hashlib.sha256(args.feed.read_bytes()).hexdigest() if args.feed else None,
        "captured_feed_generated_at": captured.get("meta", {}).get("generated_at"),
        "captured_latest_announced_at": (captured.get("data", {}).get("latest_reset") or {}).get("announced_at"),
        "captured_feed_battery": "Unavailable placeholder; no device battery measurement",
        "audits_passed": result.returncode == 0,
        "hold_preview": {
            "frames": len(hold_frames), "frame_duration_ms": 40,
            "fill_ms": 2000, "release_at_ms": 2600,
            "release_guard_ms": 250, "cleanup_first_frame_ms": 2880,
            "required_snapshots_percent": [0, 25, 50, 100],
            "rendering": "Persistent real LVGL lv_arc and geometric moon; host fixture drives model at 25 fps",
            "presentation": "Clean 240 x 320 LCD pixels, no numeric countdown or engineering overlays",
            "physical_validation": False,
            "stress": "500 in-place progress updates with stable objects and 24 KiB pool"
        },
        "source_hashes": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (ROOT / "main/reset_ui.c", ROOT / "main/reset_ui.h",
                                    ROOT / "main/reset_presenter.c",
                                    ROOT / "tools/reset_ui_preview/preview.c",
                                    ROOT / "tools/reset_ui_preview/lv_conf.h",
                                    ROOT / "tools/render_reset_preview.py",
                                    ROOT / "assets/fonts/reset_font_12.c",
                                    ROOT / "assets/fonts/reset_font_16.c",
                                    ROOT / "components/bsp/src/bsp_display_rounding.c",
                                    ROOT / "components/bsp/include/bsp_display.h")}
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if result.returncode:
        raise SystemExit("Preview renders saved, but glyph/layout audit failed; see audit.txt")
    print(f"PASS: PNG previews and provenance saved to {output}")


if __name__ == "__main__":
    main()
