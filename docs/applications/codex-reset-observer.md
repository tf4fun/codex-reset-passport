**English** · [简体中文](codex-reset-observer.zh_CN.md)

# Codex Reset Observer v2.3

Independent, read-only announcement display for the 240 × 320 AI Passport.
Data comes from [Codex Resets](https://codex-resets.com); this is not OpenAI
software or a personal quota meter. The warm paper, ink outlines, yellow time
label and pink/blue statistic cards adapt the reference site's visual style.

## Four pages and controls

Up/Down switch Announcement, Reset Overview, Statistics, and Settings. A short
Confirm on Announcement enters the full-text reader; Up/Down then turn text
pages and Confirm returns to Announcement. Confirm refreshes Overview/Statistics.
On Settings, Confirm enters its options; Up/Down select and Confirm changes or
opens the selected option.
A Confirm tap released before 500 ms performs only the ordinary short-press
operation and never opens a sleep popup. At the long-press threshold the warm
moon popup appears with an empty ring, which fills over the remaining 1.5 seconds
of the unchanged 2-second total hold. No percentage or elapsed-second number is
displayed. Releasing after the popup appears but before completion smoothly
unwinds the ring to zero over 250 ms, then closes it; that gesture never becomes
a click. All new keys during unwind/release quarantine are swallowed, and input
resumes only after stable release, preventing a rapid new tap from leaking into
a stale gesture. At 2 seconds the ring is full and asks you to release; there is
no second confirmation. Actual sleep still requires 250 ms of stable all-key
release and the existing HTTP/radio/peripheral shutdown checks.
The ring uses monotonic press/release timestamps and persistent LVGL arc objects,
updated approximately 25 times per second rather than rebuilding the page.
Other keys, focus changes or input queue overflow cancel the gesture. Provisioning
blocks sleep; an HTTP request already in flight must drain with the normal bound.
Settings has an explicit Return to Home row;
nested pages use short Confirm to save/return, and the dedicated clear-network
prompt uses short Confirm to erase or Up/Down to cancel. The separate hardware power button is not used
as an application navigation or sleep button: no readable short-press event
is established by this BSP.

Announcement chooses the newer announcement time from the latest executed reset
and an explicit scheduled reset; it does not pretend to cover every news item.
Type/status distinguish regular reset, banked-credit issuance and pending plan.
The page emphasizes relative time, with absolute date and a small type label;
announcement prose appears only after Confirm opens the full-text reader.
The expected-time panel uses scheduled_for when supplied, clearly labelled as a
plan; an elapsed schedule still awaits confirmation. Without a plan, an unexpired
watch can show its original forecast window, explicitly labelled prediction.
An expired watch is not a valid prediction and averages never predict a reset.
If neither supplies a time, the panel says it has not been announced.

Reset Overview uses Chinese relative-time wording for the latest executed event,
with local absolute date/time secondary. Banked credits are labelled separately.
The time is the site's announcement/observation record, not an independently
measured execution instant. Statistics retain total, average interval and elapsed
days; absent values remain unknown and longest interval is not invented.

Announcement text is retained as valid UTF-8, at most 256 bytes per record,
without cutting a codepoint; the forecast window is limited to 96 bytes. The
reader wraps and paginates by actual font pixel width, including unbroken URLs.
The reader keeps a simple date, page number and unobtrusive source footer.
It has an explicit maximum of eight pages; truncation/glyph-fallback notices
appear only when needed. Unsupported glyphs use safe replacement, not silent disappearance or a
full CJK font download. API text is shown in its original language, without
remote translation or hardcoded current content. The source domain is readable;
the LCD does not promise a clickable source link.

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
  two minutes; first input then only wakes the display. Active pairing/hold and
  shutdown keep the display lit.
- Network setup and confirmed network deletion are separate actions.
- Automatic deep sleep: 5/10/30 minutes or off, default 5. Only user button
  activity resets idle; API updates do not. Pairing, held keys, gestures and
  cancellation/shutdown pause the timer. Cancellation or a failed attempt adds
  a five-minute retry cooldown, rather than a tight retry loop. The preference
  is saved in NVS; old settings without the new key default to 5 without losing
  remembered Wi-Fi OFF.
- Manual deep sleep remains available through the completed hold-and-release
  gesture. Both paths share the same safe shutdown. They perform no
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
is no timer wake or immediate sleep loop. A fresh inactivity period can trigger
the enabled automatic policy; no battery-life figure is claimed.

## Pairing and security

Use the [exact companion mini-program name](../development/engineering/wifi-provisioning.zh_CN.md#mini-program-name)
and `BLUFI_FoloPassport` with a 2.4 GHz network. Setup opens only on a deliberate
button action and closes after 180 seconds, success or cancellation. Credentials
are never embedded or logged; after IP acquisition they are saved in an
application-owned ordinary NVS record, with saving reported separately from
connection. Failed connection candidates do not replace the saved network.
Storage is not additionally encrypted; the ordinary device-NVS boundary remains.

Traditional BLUFI does not authenticate peers or require encryption on every
received frame, even after DH negotiation. A nearby participant could replace
the network during the explicit window. Pair only in a trusted physical setting.
The implementation preserves the official reference protocol at commit
`9c039cc5127f22072afa83bedb7fa3d8efe635ad`; no private entry-point wrapper or
custom cryptographic protocol is used.

HTTPS uses the IDF CA bundle and hostname verification, with no redirects,
bounded JSON/body/timeouts, ETag caching and backoff. DNS, NTP and HTTPS must work;
captive portals are unsupported. Failed updates preserve the memory cache.
Announcement prose is bounded and glyph-checked; SSIDs and passwords are not displayed.

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
same 12/16 px Chinese fonts and a small 24 px relative-time subset and radius-30 BSP clipping, with repeated-page memory
checks. These are not device photos or hardware acceptance.

Device tests remain required: display/glyphs, buttons and Confirm wake/no immediate
wake, mini-program compatibility, Wi-Fi/TLS, NVS persistence, cancellation,
runtime heap and board current. A merged image is flashed at 0x0 and can reset
stored settings. Flashing requires separate approval. Earlier validated releases remain archived;
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

## Provisioning compatibility diagnostics

Use the [official Wi-Fi provisioning guide](../development/engineering/wifi-provisioning.md)
and confirm the exact entry point and failure stage. Available evidence does
not establish the cause of an “other firmware” message. EspBlufi is a public
standard-compatible client named by the reference code and is an optional user
choice; this work installs no client, impersonates no factory identity and
bypasses no client validation. V2.3 restores the official example's BLUFI error
report path: only numeric protocol codes are logged and replies require an
active provisioning connection. This is a diagnostic improvement, not a claim
to have fixed that message. No password or SSID enters the diagnostic log.

## Network recovery after restart

In the pinned ESP-IDF 5.5.3 ESP32-C3 Wi-Fi library, setting an identical RAM
configuration can return success without writing NVS. Switching storage to FLASH
does not itself flush the validated RAM configuration. Static-library call-chain
evidence supports this failure mechanism, but does not replace device logs or
reboot testing.

This revision uses a bounded application-owned NVS record with version, length
and integrity checks, committed and read back only after a candidate obtains IP.
The driver stays in RAM mode; a missing application record permits legacy driver
configuration loading. Explicit clear writes an empty record so legacy data cannot
reappear. This is logical forgetting, not secure erasure: legacy SDK records
and old flash pages may remain, and older firmware can still read its own store.
NVS writes occur during set_blob in this SDK; commit/readback failures do not
promise rollback of durable bytes. The old in-memory network remains available,
and unverified storage is reported rather than falsely confirmed.
Save failures are reported independently from a live connection; an
initialization failure can be retried by switching Wi-Fi off and on. The setup
page distinguishes absent credentials, connection progress and numeric failures,
without displaying SSIDs, passwords or keys.

If previous firmware never persisted the network before restarting, this update
cannot recover that lost credential. One fresh provisioning may be necessary;
then verify both ordinary restart and deep-sleep wake on the device.
