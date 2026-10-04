<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Actual LVGL reset UI preview

The host harness directly compiles firmware `main/reset_ui.c`,
`reset_presenter.c`, `reset_text.c`, the bounded feed parser and production
`reset_hold.c`. It renders with pinned LVGL 9.5 at 240 × 320 RGB565, a 40-row
partial buffer and the unchanged 24 KiB LVGL pool. The flush callback calls the
actual BSP rounded-row helper with radius 30 from `bsp_display.h`. This is not
an HTML recreation, device photograph, network request or flash operation.

## Run

Requires a C/C++ compiler, CMake, Python with Pillow and resolved managed LVGL
sources. From the repository root:

```bash
python3 tools/render_reset_preview.py
python3 tools/render_reset_preview.py --feed /path/to/captured-status.json
```

The optional feed must already exist locally. The real firmware parser checks
it; `generated_at` is the reference clock and battery is unavailable. Captured
screens are named `00-captured-*` (home, reader pages and overview). They are not
a current live query. All other screens use clearly labeled synthetic fixtures.
Use `--build-dir` and a fresh `--output` directory as needed. Existing output
files are retained. Neither command changes managed LVGL files.

## Announcement and reader contract

The four top-level pages are announcement, last executed reset overview,
statistics and settings. HOME short-confirm opens the reading child. Reading
UP/DOWN changes text pages; short-confirm returns HOME. Hold-to-sleep remains
independent. Settings has seven rows, including automatic sleep after 5 minutes
by default, with 0/off, 5, 10 and 30-minute values.

- HOME chooses the newer `announced_at` from `latest_reset` and
  `scheduled_reset`, never a watch's `observed_at`. The time-only HOME shows
  Chinese relative time as the hero, a secondary absolute local date, and a
  small reset type/status. It has no body summary or dense reader instructions.
- Expected timing uses a supplied schedule, explicitly labeled as planned and
  shown in the configured fixed UTC offset. Missing schedule times stay pending;
  overdue schedules await confirmation. Without a schedule, only an unexpired
  forecast's raw `forecast_window` is presented as a prediction. An expired
  forecast is not a valid plan. Average intervals never create a forecast.
- Overview uses the latest executed record: regular CODEX quota reset or
  explicitly banked/backup-quota distribution. Exact local date/time is secondary
  to Chinese relative time. The note says the time follows the site's record;
  announcement/observation time is not claimed as precise execution time.
- Text is bounded to 256 source UTF-8 bytes. Actual active LVGL glyph advances,
  including pair kerning, fit 178 pixels. Whitespace is preferred; unbroken
  words/URLs break at complete codepoint boundaries. CR/LF and TAB are normalized.
- HOME short-confirm still opens the reader. The fixed 16px reading body uses
  seven lines per page, at most eight pages. Only a quiet date, small page number
  and domain surround the text; a notice appears only when required. Byte/page clipping has
  an explicit notice. Unsupported glyphs become `?` with a visible warning.
  The original text is never translated, executed or treated as markup.
- Bounded static buffers own the label strings. No text heap allocation, full
  CJK font, remote translation, clickable-link or QR promise is introduced.
  The readable feed-site domain is `codex-resets.com`.

The 12/16px fonts cover the fixed UI and printable ASCII. A 25-glyph 24px subset
is derived directly from the Chinese relative-time formatter. Regeneration and
license information are in [the assets record](../../assets/README.md#codex-reset-observer-ui-subset).

## What the audits prove

The harness checks both body-font inventories, the hero subset, known missing
U+9F98, every visible label's actual selected font, line widths, reserved body
line budgets and screen/parent bounds. Fixtures cover:

- HOME, every top/child page, 7-row settings, offline/stale data and network errors
- new scheduled announcement, banked/regular executed or planned states,
  overdue/null schedule, raw prediction, watch-only, expired and clipped forecasts
- all four 256-byte wide-word pages, all two unbroken-URL pages,
  emoji/noncovered Chinese substitution, CR/LF/TAB and source-byte truncation
- all eight newline-heavy pages, explicit page-limit notice and index clamping
- Chinese relative times from just now to 99 days/23 hours and beyond 9999 days

The reader runs 500 page/truncation changes after the identical warm-up and
requires exact live-allocation/free-space equality. Four-top-page navigation is
also replayed for 1000 measured changes. Two successive 500-change endpoints
are each 4664 free bytes at 208 live blocks in the diagnostic run. Each measured phase
must match its own warmed phase exactly; there is no growth tolerance.
The final v2.3 run audited 90248 visible labels: zero missing glyphs and zero
clipped labels. The unchanged 24 KiB pool peaked at 17712 bytes; final top-page
free space was 4664 bytes. These are host-pool results, not device heap results.

PNG/PPM files, `synthetic-states-contact-sheet.png`, `audit.txt`, source hashes
and `manifest.json` are saved by default under `build/reset-ui-preview/screens/`.
A nonzero exit reports failure even when screenshots exist. Inspect the actual
screens after changes. Device brightness, battery ADC, physical buttons, BLE,
Wi-Fi/HTTPS pressure, credentials and on-device Chinese display remain untested
by this harness. A host render is not `Device tests: PASS`.

## Setup-only Wi-Fi diagnostics

The setup screen distinguishes starting, initialization failure, no saved
network, connecting attempt `n/5`, retry waiting, and connected-but-save-failed.
Settings shows the corresponding brief Wi-Fi state. Diagnostic lines display
numeric codes only: `E` is an error code and `D` is the last disconnect reason.
They contain no SSID or password and do not assert a root cause. Runtime and
credential-persistence errors are independent and can be shown together.
An active provisioning countdown stays visible even if an IP is acquired while
credential saving fails. The time-only HOME and seven-line reader are unchanged.
Fixtures `37`–`45` include INT32_MIN/MAX, UINT16_MAX, both errors together,
provisioning countdown plus save failure, and all startup/retry states.

## v2.3 hold-to-sleep preview

The three clean 240 × 320 GIFs use actual production press/release/tick events:

- `v2.3-fast-tap-no-popup-actual-lvgl.gif`: a 200ms tap emits one TAP, opens the
  announcement reader and never creates a sleep popup.
- `v2.3-release-1250ms-unwind-actual-lvgl.gif`: appears at 500ms, fills for the
  next 750ms, then reverses its last displayed value over 250ms. Returns HOME
  without a TAP or sleep event.
- `v2.3-full-hold-release-cleanup-actual-lvgl.gif`: appears at 500ms and fills
  during the next 1500ms. Release at 2000ms plus 250ms continuously observed
  all-keys-up produces exactly one sleep intent and enters network cleanup.

Each starts with 400ms of HOME. Frames are sampled every 20ms, with exact event
boundaries also sampled (sometimes 10ms). GIF merging of identical frames is
allowed only with preserved dimensions and total encoded duration. The cancel
fixture preserves progress 493/1000 from 1240ms instead of advancing on release.
`hold-scenarios.json` records timings/events; `hold-to-sleep-progress-strip.png`
shows the 0/25/50/100 fill states without numbers on the UI.

The warm cream/ink modal, geometric crescent and persistent LVGL arc are
unchanged. No framebuffer snapshot or countdown is used. The underlying page
is retained and hidden during the modal, then restored. Progress stress checks
500 frames with stable object identity/count, zero deletions, exact warmed
allocation equality and allocator integrity. Another 100 cancel/open-close
cycles check the hidden tap window and unchanged underlying objects. Final
progress had 21 objects, 216 allocations and 4744 bytes free before/after;
cancel-close had 186 allocations and 6496 bytes free before/after. Reader stress
had 149 allocations and 8832 bytes free before/after.
