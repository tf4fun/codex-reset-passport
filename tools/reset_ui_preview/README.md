<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Actual LVGL reset UI preview

The harness compiles the actual firmware UI, presenter, bounded text layout,
feed/history parsers, UTC history helpers and production hold state machine.
It uses pinned LVGL 9.5, 240 × 320 RGB565, a 40-row partial buffer and the
unchanged 24 KiB LVGL pool. The flush callback uses the real BSP rounded-row
helper with radius 30. It performs no network request, upload or device flash.

## Run and provenance

Requires C/C++, CMake, Python with Pillow and resolved managed LVGL sources:

```bash
python3 tools/render_reset_preview.py
python3 tools/render_reset_preview.py --feed /path/to/captured-status.json
python3 tools/render_reset_preview.py --history-feed /path/to/captured-history.json
python3 tools/render_reset_preview.py --history-weeks 6 \
  --build-dir build/reset-ui-preview-six --output build/six-week-preview
```

The optional captures must already exist locally. Actual firmware parsers
validate them. History input must contain one complete captured pagination
cycle; a partial page is rejected. Response `meta.generated_at` supplies the
preview clock, and captured screens have unavailable battery. Screens named
`00-captured-*` use these captures; all others are synthetic fixtures. The tool
records source/capture hashes, timestamps and the actual compiled week count.
It is not a live query or device photograph. Use a fresh `--output` directory
when scenarios change; older output files are not deleted.

## Four pages and immutable reading selection

The four top pages are overview, UTC history calendar, statistics and settings.
There is no separate duplicate latest-reset overview page.

- HOME always places the future card above the latest-history card. If a
  scheduled record exists, its card is large, regardless of which announcement
  is newer; the latest reset becomes a short lower card. Otherwise, the upper
  card says next time is unpublished (or shows a clearly labelled prediction),
  and the latest executed record gets the large lower card. Colours always
  identify the event: future stays sky blue at either size; latest stays white
  with a yellow time emphasis, including its compact form. They do not exchange
  colours when their sizes change. The large future card has a light-blue
  time plate with a blue edge, mirroring the large latest card's title/time/
  date/timezone hierarchy without borrowing its yellow accent.
- Planned times use actual `scheduled_for` in the configured fixed UTC offset.
  Null stays unpublished. A passed planned time awaits confirmation, never
  becomes completed merely because the clock passed it.
- Latest uses Chinese relative time with exact local date/time and explicit
  regular/banked type. This is the site's announcement/observation record,
  not a claim of an exact execution instant.
- A watch never replaces a scheduled or historical primary record. Only an
  unexpired, clock-verified watch may appear in the small future card, labelled
  prediction and optionally probability. A bounded/truncated/unsupported-glyph
  prediction visibly says that material was omitted or replaced. No timestamp
  is invented from its prose. Historical average belongs only in statistics,
  labelled as historical; it is not a fixed reset period or a forecast.
- HOME short-confirm opens the large card's original text only when a record
  exists. The application captures the last successfully displayed HOME model
  before entry. The reader references the pinned current cache and freezes
  type, date, zone, truncation flags and source; live API/timezone changes cannot silently swap
  the open record. No valid snapshot means no live-feed reader fallback.
- Reading UP/DOWN changes pages and short-confirm returns HOME. Body uses
  fixed 16px text, seven lines per page, up to 160 text pages. Wrapping uses
  actual LVGL glyph advances/kerning in 178px, preferring whitespace and then
  complete UTF-8 codepoints for unbroken words/URLs. Input is at most 1120 bytes (a 280 Unicode-codepoint budget);
  CR/LF/TAB normalize safely. Missing glyphs become `?`, and byte/page clipping
  is explicitly marked. Only a quiet date/page number/source name surrounds
  normal text. There is no translation, markup execution or text heap.
- An independent final page contains the exact selected-source QR or explicitly
  labelled data-site fallback. The 192px white square reserves four modules of
  quiet zone and integral modules of at least 3px. URLs are bounded, validated,
  never cut, and retain the selected snapshot's source.

The seven settings rows, automatic-screen-off/grace behavior and warm moon
hold animation are not changed by the presentation/reader work.

## History calendar

One `RESET_HISTORY_WEEKS` compile-time value drives query, cache, presenter and
UI. Default is eight weeks; six is supported without a device setting. Columns
are UTC Monday-start weeks, rows Monday through Sunday, with UTC month labels
and date range independent of the local announcement-display offset.

- Regular: orange `#FF5C29`; banked: peach `#FFAD7C`
- Both kinds on one day: a split-colour cell, not an invented count
- Verified no reset: warm grey; unknown: white outlined; future: pale
- Only a fully validated complete API pagination cycle marks coverage known;
  pending, failed or uncovered days never become grey empty days
- Cached overlap is retained and reprojected; new uncovered days stay unknown
- Pending network/clock, loading, stale cache and failed updates are distinct

One custom-draw object paints the lighter calendar frame, all 56 (or 42) cells,
and legend keys. Cells are 14px with 21px horizontal/20px vertical pitch: 7px
horizontal and 6px vertical clear gaps. Solid dates no longer have heavy black
outlines; unknown cells retain their distinguishing outline. The entire table (weekday labels, cells and month labels) is shifted 3px
left as a group. The default eight-week actual visible bounds are x=26..210,
leaving 7px/8px inside-frame side gaps; six-week positioning centres that same
complete group. Month labels clamp to the group
right-side padding. Status sits beside the date range, leaving a spaced,
centred two-line legend. All text remains at its previous font size.
The seven weekday labels remain one multiline object at 20px row positions.
Geometry is shared with the pixel probes in `main/reset_ui_layout.h`; the
retained-background hold overlay still fits in the unchanged 24 KiB pool.

The real 8-week capture used for `00-captured-history.png` contains 16 records
in one complete page, with response generation time 2026-10-04T14:55:20.166Z.
Its displayed UTC window is 2026-08-10 through 2026-10-04. This historical capture
is distinct from synthetic mixed/unknown/future stress fixtures.

## Audits

Font coverage checks both body fonts, the tiny relative-time font, and known
missing U+9F98. Every visible label's actual font, width, reserved line budget,
screen and parent bounds are checked. Fixtures cover all four top pages,
settings/setup errors, schedule older than latest, planned null/overdue time,
watch-only/expired forecast, long words/URLs, unsupported glyphs, byte/page
limits, all 160 newline-heavy text pages and frozen reader/source across API/zone changes.

History cases include real capture, partial coverage, loading/error, future
cells, unknown clock, and all 56 cells containing both reset kinds. Independent
pixel probes verify both halves of every rendered cell against its model state.
HOME, QR and worst-case history each run 500 actual hold progress frames and
100 cancel/open-close cycles with stable objects and exact warmed allocation
counts/free bytes. QR also runs 100 reader/source page cycles and 111 actual-QR-object checks;
  an unavailable-message-only page fails rather than passing label audits. Reader changes
run 500 iterations; all four top pages run 1200 measured changes. There is no
allocator-growth tolerance.

The earlier v2.4 visual-polish default 8-week host run audited 317614 labels with zero missing glyphs
and zero clipping. All 582 HOME semantic-colour probes and 1672 calendar
colour/clear-gap probes passed. Pool peak was 18288/24576 bytes (the restored
baseline peaked at 18648). Worst-case history hold retained 23 objects and 223
live allocations with 4104 bytes free before/after; cancel returned to 193
allocations and 5864 bytes free. These are host-pool results, not ESP32 total-heap acceptance.

Independent pixel QR decoding requires Pillow and zxing-cpp:

```bash
python3 tests/test_reset_qr_decode.py build/v2.4-history-final/screens
```

It verifies six actual QR outputs: fallback, maximum URL, malformed/observed
  fallback, frozen selected source, and the captured API source. Exact URLs,
  integral 3px-or-larger modules and full quiet zone must all match.
The frozen-reader QR also decodes to the original planned source after live-feed
and timezone changes. This does not prove optical scanning on the real LCD.

Output includes PNG/PPM, a labelled synthetic contact sheet, `audit.txt`,
`manifest.json`, `hold-scenarios.json` and hold GIFs. Nonzero exit means a failed
audit even if images exist. Brightness, battery ADC, physical buttons, radio/TLS
memory pressure, credentials, LCD rendering and phone scanning remain device
tests. Host rendering is not `Device tests: PASS`.

## Preserved hold GIFs

- `v2.3-fast-tap-no-popup-actual-lvgl.gif`: 200ms ordinary TAP opens the captured
  record's reader without a popup
- `v2.3-release-1250ms-unwind-actual-lvgl.gif`: shows at 500ms, unwinds the last
  displayed progress for 250ms after release, without TAP or sleep intent
- `v2.3-full-hold-release-cleanup-actual-lvgl.gif`: fills at 2000ms total; release
  plus 250ms continuous all-keys-up emits one sleep intent and enters cleanup

Each starts with 400ms of HOME. Sampling is normally 20ms, plus exact event
boundaries. Encoded GIF dimensions and total duration are verified. The ring,
geometric crescent and warm palette remain unchanged, without numeric countdown
or framebuffer snapshot. The underlying page is retained and hidden during the
modal; object identity and allocation audits verify this behavior.

QR allocation admission uses actual I1 canvas stride × height, palette and
alignment sizes for both the constructor-default and requested canvas, because
LVGL allocates the replacement before releasing the default. It separately
budgets draw descriptors, encoder scratch and conservative object overhead.
Total free memory covers the peak; largest-block admission covers individual
allocations, avoiding the former fixed 6000-byte false rejection after capture/
history navigation. The post-canvas dual-scratch guard remains in place.

## Visual-polish regression boundary

The restored v2.4 source was rendered before the visual-only changes. Ordinary
reading, statistics, settings, the hold progress frame and frozen-source QR
PNGs remain byte-identical to that baseline. The changed screens are HOME card
colour/hierarchy/time placement and calendar spacing/frame/legend. Presenter, feed, controls, networking, screen-off behavior, QR
encoding/URL/budget logic and font assets are unchanged. Both short and full
fixture sequences still undergo the original memory and independent QR tests.

The statements above describe the earlier visual-only pass. The expanded
announcement reader now also changes text storage, cache ownership, pagination
and its page-number width; the current run must pass the same audits.

### Time-plate optical centring

The large blue future plate and both latest yellow time containers centre the
union of actual font glyph bounds using LVGL's line height, baseline, glyph
box height and y offset. Odd spare pixels round upward; this avoids depending
on the larger line box or guessed per-string offsets. The rendered-pixel check
requires top/bottom ink margins to differ by at most one pixel. All 210 checks
pass, including compact 99-day/23-hour, 9999-day, just-now, unknown-time and
no-record states. The extra future-plate widget also runs the original 500-frame
hold and 100-cancel-cycle stress with exact stable allocations. The time-plate refinement did not change history. The later, requested
margin-only adjustment moves its complete table left without changing cell
size, pitch, fonts, data or legend. Eight actual visible-bound checks require
both inner side gaps to be at least 7px and differ by at most 1px; all pass.
Both HOME-state PNGs remain byte-identical to the final time-plate revision.
