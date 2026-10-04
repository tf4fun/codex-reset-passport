<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# CI firmware verification and device waiting

[verify_ci_firmware.py](../verify_ci_firmware.py) verifies an extracted CI package
offline. [flash_ci_firmware.py](../flash_ci_firmware.py) uses the same verification,
then optionally waits for one explicitly identified board and performs a single
flash, independent verification, standard reset and bounded boot observation.
These tools do not build firmware, install dependencies, download files or change
security settings. Default operation is a dry-run with no serial access or output
files, and works without esptool or pyserial installed.

## Supported package and data preservation

Use the package produced by [package_ci_firmware.py](../package_ci_firmware.py):
`FoloToy-AI-Passport-full.bin`, `manifest.json` and `SHA256SUMS` are required.
Supply the approved SHA256 independently from the trusted CI handoff. Verification
checks the entire file, manifest sizes/hashes, complete C3 image segments,
checksums and appended SHA256, application/ELF identity, component offsets,
partition MD5, erased gaps and sector-aligned erase bounds. Component lengths
must cover complete images, and the application must reach the merged file end.
The manifest is read as data. Hash consistency is not a publisher signature or
hardware acceptance. An ELF checksum identity in the manifest does not provide
the actual matching ELF for crash decoding.

This tool intentionally supports ESP32-C3, 8MB Flash, ESP-IDF 5.5.3, DIO/80MHz and
the repository's minimal partition layout. Other valid layouts remain supported
by the repository's general build workflow; use their separately reviewed flash
workflow rather than forcing this tool past its checks.

| Component | Offset | Protection |
| --- | --- | --- |
| Bootloader | `0x0` | Must end before the protected data sectors |
| Partition table | `0x8000` | One table sector, ending before `0x9000` |
| Factory application | `0x10000` | Must fit the `0x7F0000` factory partition |
| NVS | `0x9000` through `0xEFFF` | Never read, erased or written by this tool |
| PHY | `0xF000` through `0xFFFF` | Never erased or written by this tool |

The merged file pads data gaps; writing the whole merged file can reset NVS.
This tool extracts only the three checksum-verified components from an immutable
in-memory snapshot. Before writing, it reads only the existing partition-table
sector (`0x8000`, 4096 bytes) and requires it to match the CI table. This is a
compatibility check, not an original-firmware backup. It never reads Wi-Fi
credentials. Preserving the sectors does not prove that a new application uses
the preserved data correctly; confirm application compatibility.

## Offline checks and dry-run

Activate the Python environment you intend to use. Replace the quoted
placeholders with values from the approved artifact:

```bash
python3 tools/verify_ci_firmware.py /path/to/extracted-ci \
  --sha256 '<approved SHA256>' --expected-version '<app version>'

python3 tools/flash_ci_firmware.py /path/to/extracted-ci \
  --sha256 '<approved SHA256>' --expected-version '<app version>'
```

The input may instead be the merged file path. `--manifest /path/to/manifest.json`
selects an explicit manifest; `SHA256SUMS` stays beside the merged image.
`--expected-version` is optional and must match the binary when supplied.
Without it, boot verification uses the version already verified in the binary.

## Authorized execution

Execution currently supports macOS/Linux with an existing official esptool
5.4.x environment, pyserial and `lsof`. Use that environment's Python for the
command. Identify the target's native USB serial beforehand with the read-only
`python3 -m serial.tools.list_ports -v`; the C3 USB serial is its MAC identity.
Keep the USB connection stable. Seeing a connected device is not flash consent;
confirm the target, approved image and data scope with its owner before execution.

```bash
python3 tools/flash_ci_firmware.py /path/to/extracted-ci \
  --sha256 '<approved SHA256>' --expected-version '<app version>' \
  --execute --target-serial '<native USB serial>' \
  --wait-seconds 600 --output build/device-flash/run-001
```

`--execute` requires a known target serial and a new output directory. USB VID/PID
and identity must match uniquely; port names are rediscovered after resets.
Serial ownership is checked without killing other processes. A private target
lock prevents concurrent runs for the same board. Existing receipt directories
are rejected, and stale target locks are not automatically cleared.

After offline preparation and locking, the tool prints `waiting_for_wake` and the
UTC deadline. At that point the owner can wake the board. The default window is
600 seconds, configurable from greater than zero through 3600 seconds. An already
present target proceeds immediately. Detection, security/capacity/partition
preflight, the single write, independent verify and boot observation run
continuously without another prompt or a model turn. A missing or ambiguous
target, unknown/enabled security flags, wrong chip/capacity or changed partition
table stops the process before writing.

`--command-seconds` defaults to 120 (maximum 180); `--boot-seconds` defaults to
25 (maximum 60). Firmware writes use stock esptool safety checks. A bounded child
process disables esptool's image and block retries for that invocation and ignores
inherited `ESPTOOL_*` defaults; it does not alter installed files or configuration.
There is no
whole-chip erase, force flag, encryption bypass, automatic write retry or
automatic recovery reset after failure. Verification uses original component
bytes with explicit `keep` parameters. Stub commands use `no-reset-stub` to avoid
the recovery reset implied by `no-reset`. Only after verification passes does the
tool perform one standard reset and collect startup logs.

## Results and stopped operations

`result.json` records `preflight`, `write`, `verify`, `boot`, the write-attempt
count and data boundaries. Stage output is printed as JSON. Write intent is
persisted before the command; an interrupted, failed or timed-out write is
`UNCERTAIN` and stops all subsequent verify/reset/boot operations. Review the
receipt, process state and device before deciding on a new manual operation;
never interpret an execution-channel disconnect as permission to launch again.
If a process died leaving a target lock, establish that it has ended and inspect
its receipt before manually removing that lock.

Boot PASS requires the expected application version, `app_main()` and no detected
panic in the observation window. English and Chinese BSP readiness messages are
recognized separately. A silent, wrong-version or crashing boot exits nonzero;
it does not reflash. The monitor closes in `finally`, including read errors.
Only allowlisted, sanitized boot lines are saved; credential/SSID/token/URL lines
are discarded and MAC/IP values are redacted. Subprocess output is sanitized
before saving. Keep receipts, component binaries and runtime logs outside Git;
the example uses the ignored `build/` directory.

Success exits `0`; validation, timeout, command or boot failure exits nonzero.
Build and host tests belong to the originating CI. Successful flashing/boot does
not validate LCD appearance, button interactions, camera QR scanning, RF behavior,
deep-sleep/wake, stored-settings semantics or power consumption.

## Tests without hardware

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_flash_ci_firmware.py
```

The tests use synthetic images and mocked discovery/esptool/serial operations.
They cover hash/mapping/erase-boundary failures, dry-run isolation, absent and
ambiguous devices, bounded waiting, security/capacity/partition rejection,
complete-image checks, child retry controls, single-write failure handling,
independent verification, success ordering,
redaction, Chinese boot evidence, output reuse and monitor closure. They require
no device or SDK and run in the existing full repository CI gate.
