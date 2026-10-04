<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Actual LVGL reset UI preview

This is a host-only renderer and check harness for the firmware presentation.
It directly compiles `main/reset_ui.c`, `main/reset_presenter.c`, the bounded
`main/reset_feed.c` parser, and the same generated Chinese font as firmware.
It uses the pinned managed LVGL 9.5 source, a 240 × 320 RGB565 display, a 40-row
partial draw buffer, and the same 24 KiB LVGL allocation pool size. The flush
callback calls the actual pure BSP rounded-row helper and reads radius 30 from
`bsp_display.h`, filling clipped corners with black exactly as the BSP does. No web page,
HTML recreation, USB access, network request, or device flashing is involved.

## Run

Prerequisites: a C/C++ compiler, CMake, Python 3 with Pillow, and the resolved
`managed_components/lvgl__lvgl` source. From the repository root:

```bash
python3 tools/render_reset_preview.py
python3 tools/render_reset_preview.py --feed /path/to/captured-status.json
```

The second command is optional. It reads an existing local response from
`https://codex-resets.com/api/v1/status`; it does not fetch or upload anything.
The firmware parser validates it before use. Set `--output` and `--build-dir`
when different generated paths are needed. Nothing modifies firmware source
or managed LVGL files. Activate your host/ESP-IDF environment if CMake is not
on `PATH`.

## What the images prove

- `00-captured-feed.png`, when requested, uses real captured data and the
  response's `generated_at` time as the reference clock. Its battery is an
  unavailable placeholder. It is not a device photograph or a current live
  query; see the API capture time and SHA-256 in the provenance files.
- `01` through `17` are deterministic synthetic fixtures: fresh home, details,
  Bluetooth setup, destructive-action confirmation, offline cached data,
  first offline boot, overdue schedule, AI prediction, network error, and
  banked/observed reset details, pending banked reset, and an initial rate limit. The contact sheet explicitly labels them as
  synthetic and shows each screen at its native 240 × 320 resolution.
- Actual label bindings are checked for missing glyphs, single-line clipping,
  and screen-bound overflow. All fixed font-inventory codepoints are checked
  against the generated LVGL font; known-missing U+9F98 must be rejected.
- Duplicate updates and 500 page changes exercise the real presentation code
  in a 24 KiB LVGL pool. The audit reports peak use, remaining memory, largest
  free block, and fragmentation. Host pointer sizes/draw behavior can differ
  from the ESP32-C3. This is not a claim about the device's total free heap.

Output is in `build/reset-ui-preview/screens/` by default: individual PNG/PPM
files, `synthetic-states-contact-sheet.png`, `audit.txt`, and `manifest.json`.
The manifest includes presentation/font source hashes, captured-response hash,
and check status. A nonzero exit reports font/layout failure even when images
are available. Check the final images after UI source changes. Existing files
in a reused output folder are retained, so use a fresh `--output` directory
when the requested scenario set changes.

Device display, brightness, battery ADC readings, buttons, real Bluetooth/Wi-Fi,
HTTPS/TLS memory pressure, credentials, and physical Chinese rendering remain
separate hardware tests. Host rendering is not `Device tests: PASS`.

## Font source and regeneration

See [the assets font record](../../assets/README.md#codex-reset-observer-ui-subset)
for Noto CJK source/version/hash, OFL license, fixed-string inventory, and exact
pinned converter commands. `tools/generate_reset_font.py --check` needs only the
Python standard library: it checks current source text against the inventory and
verifies the generated C cmap coverage and descriptor bounds. The runtime LVGL
check is separate and also inspects actual widget font bindings. The firmware never needs Python,
Pillow, CMake host tools, the source OTF, or the font converter at runtime.

## v2.2 hold-to-sleep preview

`hold-to-sleep-actual-lvgl.gif` records the real firmware `lv_arc` at 40 ms
intervals (25 fps): 0–100% over 2000 ms, the armed “release to sleep” state,
the 250 ms stable-release guard, and the existing network-cleanup page.
The last guard frame is sampled at 2840 ms and the first cleanup frame at
2880 ms, after a release at 2600 ms. This samples the guard at 25 fps; it does
not redefine the firmware's 250 ms threshold. Identical GIF frames may merge
into one longer frame. The GIF is the clean 240 × 320 LCD render, without percentage/countdown text
or engineering captions. Host-only provenance is recorded in `manifest.json`.
`hold-to-sleep-progress-strip.png` shows four fill stages without numeric labels.
The title is “prepare to rest”; before the ring fills the hint is “release to return”, and
once armed it is “release to sleep”. A crescent is drawn from two small geometric
discs in the center; no raster asset or additional moon widget is allocated.
Additional PNGs cover cancelled release, stable-release waiting, and provisioning
blocking. There is no second standalone sleep-confirmation page.

A popup creates its widgets once per press gesture. Each progress update keeps
the same arc, labels, and underlying page objects. While the modal is visible,
the underlying page's drawing is hidden behind a quiet cream/ink backdrop;
closing the popup deletes only its objects and restores the page. Read-only
shared text styles, static-lifetime text buffers, and draw-event card shadows
keep the unchanged 24 KiB LVGL pool sufficient. No full-screen image is cached.

The harness warms arc angles and drawing scratch buffers, then verifies 500 updates
with stable object identity/count, zero object deletion, unchanged allocation
count/free space, and a valid allocator. It also retains 500 page-change checks.
For the v2.2 fixture, the hold check has 24 objects, 234 allocation blocks,
3536 bytes free before/after, and a 18712-byte recorded peak across all tests.
1454 rendered labels pass with zero missing glyphs and zero clipped labels.
These are host-pool results, not device heap or physical-button acceptance.
