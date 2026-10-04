**English** · [简体中文](codex-reset-observer.zh_CN.md)

# Codex Reset Observer v2.2

Independent, read-only announcement display for the 240 × 320 AI Passport.
Data comes from [Codex Resets](https://codex-resets.com); this is not OpenAI
software or a personal quota meter. The warm paper, ink outlines, yellow time
label and pink/blue statistic cards adapt the reference site's visual style.

## Four pages and controls

Up/Down switch Overview, Statistics, Announcement, and Settings. Confirm
requests a refresh on the first three pages. On Settings, Confirm enters its
options; Up/Down select and Confirm changes or opens the selected option.
Pressing Confirm immediately opens a lightweight circular-progress popup with
a small geometric moon. No percentage or elapsed-second number is displayed.
Release before 500 ms to perform the normal short-press action. Release from
500 ms up to (but not including) 2 seconds to cancel without performing that
action. At 2 seconds the ring is full and asks you to release; there is no second
confirmation. Actual sleep starts only after all keys remain released for
250 ms, followed by the existing HTTP/radio/peripheral shutdown checks.
The ring uses monotonic press/release timestamps and persistent LVGL arc objects,
updated approximately 25 times per second rather than rebuilding the page.
Other keys, focus changes or input queue overflow cancel the gesture. Provisioning
blocks sleep; an HTTP request already in flight must drain with the normal bound.
Settings has an explicit Return to Overview row;
nested pages use short Confirm to save/return, and the dedicated clear-network
prompt uses short Confirm to erase or Up/Down to cancel. The separate hardware power button is not used
as an application navigation or sleep button: no readable short-press event
is established by this BSP.

Overview shows time since the announcement/observation, its absolute date and
time, and explicit timezone. Regular resets and banked credits stay distinct.
A schedule stays pending after its date; AI forecasts are not promises.
Statistics show the API's total, mean interval and elapsed days, with missing
values shown as unknown. The API does not expose the website's longest interval,
so the device does not invent it. Details show source and data-generation UTC
time separately from the last successful request.

## Settings

- Wi-Fi ON/OFF is saved. OFF preserves credentials and prevents scans, BLE setup,
  retries and automatic reconnection; turning it on reconnects the saved network.
- Automatic updates: 5/15/30/60 minutes, default 15. Manual and wake requests can
  bypass that interval after a 30-second debounce, but never failure backoff or
  server Retry-After. Remaining wait is visible. Five minutes is not an official
  published rate limit.
- Local time: explicit fixed UTC offset in 15-minute steps, from UTC−12 to UTC+14.
  Default UTC; no guessed location and no automatic daylight-saving changes.
- Brightness: 20/45/75%. Normal viewing dims after 30 seconds and blanks after
  two minutes; first input then only wakes the display. Setup/settings stay lit.
- Network setup and confirmed network deletion are separate actions.
- Deep sleep is manual, entered through the completed Confirm hold-and-release gesture. It performs no
  periodic wake or background update. Bottom Confirm is the primary intended wake key, **not yet
  board-verified**; release all keys before sleeping. Use the hardware power
  control if wake fails. All three function keys share GPIO0, so Up/Down may also wake it; the
  firmware cannot identify which resistor-ladder key caused the LOW wake.

Before sleep, HTTP drains and radio shutdown must acknowledge completion.
Buttons transfer from ADC to digital GPIO only after stable release. Codec,
battery gauge, buses and display are shut down in board order; failures after
irreversible shutdown restart safely. Sleep cancellation restores services and
waits for key release. A checked RTC cache survives deep sleep, not power loss.
Wake displays old data first, then reconnects only if Wi-Fi is enabled, obtains
an actual SNTP synchronization and revalidates while preserving backoff. There
is no automatic return to deep sleep and no claimed battery-life figure.

## Pairing and security

Use the [exact companion mini-program name](../development/engineering/wifi-provisioning.zh_CN.md#mini-program-name)
and `BLUFI_FoloPassport` with a 2.4 GHz network. Setup opens only on a deliberate
button action and closes after 180 seconds, success or cancellation. Credentials
are never embedded or logged; they persist in ordinary device NVS after IP
acquisition. Failed candidates do not replace the saved network.

Traditional BLUFI does not authenticate peers or require encryption on every
received frame, even after DH negotiation. A nearby participant could replace
the network during the explicit window. Pair only in a trusted physical setting.
The implementation preserves the official reference protocol at commit
`9c039cc5127f22072afa83bedb7fa3d8efe635ad`; no private entry-point wrapper or
custom cryptographic protocol is used.

HTTPS uses the IDF CA bundle and hostname verification, with no redirects,
bounded JSON/body/timeouts, ETag caching and backoff. DNS, NTP and HTTPS must work;
captive portals are unsupported. Failed updates preserve the memory cache.
Raw server prose, SSIDs and passwords are not displayed.

## Build and validation

Activate ESP-IDF 5.5.3, then explicitly apply the pinned, application-specific
BLUFI receive-fragment bounds correction:

```bash
python3 tools/apply_blufi_safety_patch.py --apply
./tools/validate.sh
```

The tool checks SDK version and exact source hashes; unknown changes are refused
and repeat application is harmless. The patch/manifest live in `tools/patches/`.
It bounds short fragments and 1025-byte reassembly, matching this application's
maximum DH message; it changes no encryption semantics. It is not a generic
upstream fix. Other projects sharing the SDK also see this local patch; use a
separate clean SDK for an unmodified baseline. The firmware gate verifies the
patch and uses a fresh configuration, including disabled internal sleep GPIO
resistors for the external-pull-up button ladder.

Host tests cover parsing, HTTP/RTC/backoff, navigation/settings/time conversion,
Wi-Fi OFF/late events, button ADC handoff, sleep sequencing and SDK fragment
bounds. [Actual LVGL previews](../../tools/reset_ui_preview/README.md) use the
same 12/16 px Chinese fonts and radius-30 BSP clipping, with repeated-page memory
checks. These are not device photos or hardware acceptance.

Device tests remain required: display/glyphs, buttons and Confirm wake/no immediate
wake, mini-program compatibility, Wi-Fi/TLS, NVS persistence, cancellation,
runtime heap and board current. A merged image is flashed at 0x0 and can reset
stored settings. Flashing requires separate approval. V1, v2 and v2.1 remain archived;
no automatic update or device flash is performed by this build.

### Confirm wake electrical basis

The board defines Confirm as a 2.2 kOhm pull-down against a 10 kOhm external
pull-up: approximately 595 mV at 3.3 V. The ESP32-C3 input-low maximum is
0.25 times VDD (825 mV at 3.3 V), so the nominal voltage supports LOW wake.
Up and Down are also nominally LOW. This calculation is not a measured wake
test; the awake ADC acceptance window is wider than the digital LOW guarantee.
See the [official ESP32-C3 datasheet, DC characteristics](https://documentation.espressif.com/ESP32-C3_Datasheet_en.pdf).
The existing stable-release/ADC-to-GPIO handoff is retained. After wake, input
is blocked until 250 ms of stable release, longer than the 180 ms short-click
delay, so the wake press cannot refresh or immediately reopen sleep.
