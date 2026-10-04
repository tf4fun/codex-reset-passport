#!/usr/bin/env python3
"""Plan, or explicitly execute, one NVS-preserving CI firmware flash after wake."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from archive_firmware import safe_path
from verify_ci_firmware import Firmware, require, sha256, verify_package


SENSITIVE = re.compile(r"password|passwd|passphrase|credential|secret|token|ssid|bssid|\bpsk\b|"
                       r"authorization|bearer|api[_ -]?key|https?://|otpauth:", re.I)
CRASH = re.compile(r"Guru Meditation|panic(?:.ed|:)|assert failed|Backtrace:|abort\(|stack smashing|"
                   r"CORRUPT HEAP|Brownout detector|watchdog got triggered|Interrupt wdt timeout", re.I)
BOOT_LINES = re.compile(r"ESP-ROM:|^Build:|^rst:|^Saved PC:|^SPIWP:|^mode:|^load:|^entry |"
    r"\b(?:boot[\w.]*|esp_image|cpu_start|app_init|main_task|heap_init|spi_flash|bsp_\w+|reset_\w+):", re.I)
# esptool 5.4 otherwise retries entire images and uncertain blocks internally.
# These process-local settings never modify the installed package or its config.
ESPTOOL_CHILD = """import esptool
import esptool.loader as loader
if not str(esptool.__version__).startswith('5.4.'):
    raise RuntimeError('execution requires esptool 5.4.x')
if not hasattr(loader, 'WRITE_BLOCK_ATTEMPTS') or not hasattr(loader.ESPLoader, 'WRITE_FLASH_ATTEMPTS'):
    raise RuntimeError('unknown esptool retry controls')
loader.WRITE_BLOCK_ATTEMPTS = 1
loader.ESPLoader.WRITE_FLASH_ATTEMPTS = 1
esptool.main()
"""


def normalize(line: str) -> str:
    line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line)
    return "".join(char for char in line if ord(char) >= 32 or char == "\t")


def sanitize(line: str) -> str | None:
    line = normalize(line)
    if SENSITIVE.search(line):
        return None
    line = re.sub(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", "[MAC redacted]", line, flags=re.I)
    line = re.sub(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", "[IP redacted]", line)
    return line[:2048]


def clean_output(text: str) -> str:
    return "\n".join(value for line in text.splitlines() if (value := sanitize(line)) is not None)


def serial_identity(value: str) -> str:
    normalized = value.replace(":", "").replace("-", "").upper()
    require(re.fullmatch(r"[0-9A-F]{12}", normalized) is not None,
            "target USB serial must identify the native C3 MAC, with optional colon separators")
    return normalized


def select_target(ports: list, identity: str):
    selected = []
    for port in ports:
        if port.vid != 0x303A or port.pid != 0x1001 or not port.serial_number:
            continue
        try:
            match = serial_identity(port.serial_number) == identity
        except ValueError:
            match = False
        if match:
            selected.append(port)
    require(len(selected) <= 1, "multiple matching USB targets; no arbitrary port selection")
    return selected[0] if selected else None


@dataclass(frozen=True)
class Options:
    identity: str
    wait_seconds: float = 600
    boot_seconds: float = 25
    command_seconds: float = 120


def wait_target(backend, identity: str, seconds: float, clock=time.monotonic, sleep=time.sleep):
    end = clock() + seconds
    while True:
        port = select_target(backend.ports(), identity)
        if port is not None:
            backend.check_owner(port.device)
            return port
        remaining = end - clock()
        if remaining <= 0:
            raise TimeoutError("target did not appear before the waiting deadline")
        sleep(min(0.25, remaining))


def security_check(text: str, identity: str) -> None:
    flags = re.search(r"Flags:\s*(0x[0-9a-f]+)", text, re.I)
    chip = re.search(r"Chip ID:\s*(\d+)", text)
    mac = re.search(r"MAC:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})", text, re.I)
    require(flags is not None and int(flags[1], 16) == 0, "unsafe or unknown security flags")
    require(chip is not None and int(chip[1]) == 5 and "ESP32-C3" in text, "target is not ESP32-C3")
    require("Secure Boot: Disabled" in text and "Flash Encryption: Disabled" in text,
            "secure boot and flash encryption must be disabled")
    require(mac is not None and serial_identity(mac[1]) == identity, "chip identity differs from selected USB target")


def boot_evidence(lines: list[str], version: str) -> tuple[dict, str]:
    lines = [normalize(line) for line in lines]
    crashed = any(CRASH.search(line) is not None for line in lines)  # Before redaction.
    kept = [value for line in lines if (BOOT_LINES.search(line) or CRASH.search(line))
            and (value := sanitize(line)) is not None]
    text = "\n".join(kept) + "\n"
    version_seen = re.search(r"(?:App|Project) version:\s*" + re.escape(version) + r"(?:\s|$)", text) is not None
    app_seen = "Calling app_main()" in text
    return {"boot": "PASS" if version_seen and app_seen and not crashed else "FAIL",
            "version_observed": version_seen, "app_main_observed": app_seen, "crash_observed": crashed,
            "display_ready": re.search(r"bsp_(?:disp|display):.*(?:Display.*ready|显示就绪)", text, re.I) is not None,
            "lvgl_ready": re.search(r"bsp_lvgl:.*(?:ready|started|就绪)", text, re.I) is not None,
            "buttons_ready": re.search(r"bsp_(?:btn|button):.*(?:ready|按键就绪)", text, re.I) is not None}, text


class EsptoolBackend:
    def __init__(self):
        require(sys.platform in ("darwin", "linux"), "execution supports macOS and Linux only")
        require(importlib.util.find_spec("esptool") is not None and importlib.util.find_spec("serial") is not None,
                "activate an existing esptool/pyserial environment; this tool installs nothing")
        import esptool
        import serial
        from serial.tools import list_ports
        require(str(esptool.__version__).startswith("5.4."), "execution currently supports esptool 5.4.x")
        self.serial = serial
        self.list_ports = list_ports
        self.serial_closed = True
        self.lsof = shutil.which("lsof")
        require(self.lsof is not None, "lsof is required to check serial ownership")

    def ports(self):
        return list(self.list_ports.comports())

    def check_owner(self, port: str) -> None:
        paths = [port]
        if sys.platform == "darwin" and port.startswith("/dev/cu."):
            paths.append(port.replace("/dev/cu.", "/dev/tty.", 1))
        owners = subprocess.run([self.lsof, "-t", *paths], capture_output=True, text=True, timeout=8)
        require(owners.returncode in (0, 1) and not owners.stdout.strip(), "serial port is busy or ownership is unknown")

    def call(self, port: str, name: str, arguments: list[str], before: str,
             after: str, no_stub: bool, timeout: float) -> str:
        command = [sys.executable, "-c", ESPTOOL_CHILD, "--chip", "esp32c3", "--port", port,
                   "--baud", "115200" if name == "run" else "460800", "--before", before, "--after", after]
        if no_stub:
            command.append("--no-stub")
        environment = {key: value for key, value in os.environ.items() if not key.startswith("ESPTOOL_")}
        process = subprocess.run([*command, name, *arguments], capture_output=True, text=True,
                                 timeout=timeout, env=environment)
        raw = process.stdout + process.stderr
        self.last_output = clean_output(raw)
        if process.returncode != 0:
            raise RuntimeError(name + " failed; stopped without retry")
        return raw

    def observe(self, identity: str, seconds: float) -> tuple[list[str], bool]:
        deadline = time.monotonic() + seconds
        monitor = None
        pending = b""
        lines = []
        received = 0
        try:
            while time.monotonic() < deadline:
                if monitor is None:
                    port = select_target(self.ports(), identity)
                    if port is None:
                        time.sleep(0.1)
                        continue
                    self.check_owner(port.device)
                    monitor = self.serial.Serial(port=None, baudrate=115200, timeout=0.2,
                                                 rtscts=False, dsrdtr=False)
                    monitor.dtr = False
                    monitor.rts = False
                    monitor.port = port.device
                    self.serial_closed = False
                    monitor.open()
                    monitor.dtr = True
                    monitor.rts = False
                data = monitor.read(4096)
                received += len(data)
                require(received <= 1024 * 1024, "boot observation exceeded its bounded input budget")
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    lines.append(line.decode("utf-8", "replace").rstrip("\r"))
                require(len(pending) <= 16384, "boot log line exceeded its bounded input budget")
            if pending:
                lines.append(pending.decode("utf-8", "replace"))
            return lines, True
        finally:
            if monitor is not None:
                try:
                    monitor.dtr = False
                    monitor.rts = False
                finally:
                    monitor.close()
                    self.serial_closed = not monitor.is_open


@contextmanager
def target_lock(identity: str):
    root = safe_path(Path(tempfile.gettempdir()) / f"passport-flash-locks-{os.getuid()}")
    root.mkdir(mode=0o700, exist_ok=True)
    require(root.stat().st_uid == os.getuid() and root.stat().st_mode & 0o077 == 0,
            "target lock directory must be private and owned by the caller")
    lock = root / (hashlib.sha256(identity.encode()).hexdigest() + ".lock")
    handle = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        os.write(handle, str(os.getpid()).encode())
        yield
    finally:
        os.close(handle)
        lock.unlink()


def execute(firmware: Firmware, options: Options, output: Path, backend,
            clock=time.monotonic, sleep=time.sleep, emit=print) -> dict:
    require(0 < options.wait_seconds <= 3600 and 0 < options.boot_seconds <= 60
            and 0 < options.command_seconds <= 180, "timeouts must be positive and bounded")
    identity = serial_identity(options.identity)
    output = safe_path(output)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)  # Never overwrite an earlier receipt.
    result = {"firmware": firmware.summary(), "preflight": "NOT RUN", "write": "NOT RUN",
              "verify": "NOT RUN", "boot": "NOT RUN", "write_attempts": 0,
              "nvs_written": False, "phy_written": False, "whole_chip_erase": False,
              "serial_closed": True, "device_behavior_tests": "NOT RUN"}
    def report(phase: str):
        result["phase"] = phase
        temporary = output / "result.tmp"
        temporary.write_text(json.dumps(result, indent=2) + "\n")
        temporary.replace(output / "result.json")
        emit(json.dumps({"phase": phase, "deadline_utc": result.get("deadline_utc"),
                         **{k: result[k] for k in ("preflight", "write", "verify", "boot")}}))
    def fresh_port():
        return wait_target(backend, identity, min(5, options.wait_seconds), clock, sleep).device
    def call(name: str, arguments=None, before="no-reset", after=None, no_stub=True):
        port = fresh_port()
        # no-reset restarts a loaded stub; no-reset-stub leaves it alone on errors.
        after = after or ("no-reset" if no_stub else "no-reset-stub")
        raw = backend.call(port, name, arguments or [], before, after, no_stub, options.command_seconds)
        (output / f"esptool-{name}.log").write_text(clean_output(raw) + "\n")
        return raw
    try:
        with target_lock(identity):
            paths = []
            for part in firmware.components:
                path = output / part.name
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(part.data)
                path.chmod(0o400)
                paths.extend([hex(part.offset), str(path)])
            result["deadline_utc"] = (datetime.datetime.now(datetime.timezone.utc)
                                      + datetime.timedelta(seconds=options.wait_seconds)).isoformat()
            report("waiting_for_wake")
            wait_target(backend, identity, options.wait_seconds, clock, sleep)
            result["preflight"] = "IN PROGRESS"
            report("preflight")
            security_check(call("get-security-info", before="usb-reset"), identity)
            require("Detected flash size: 8MB" in call("flash-id"), "device flash capacity must be 8MB")
            table_path = output / "device-partition-table.bin"
            call("read-flash", ["0x8000", "0x1000", str(table_path)], no_stub=False)
            table = table_path.read_bytes()
            expected_table = next(part.data for part in firmware.components if part.offset == 0x8000)
            require(len(table) == 4096 and table[:len(expected_table)] == expected_table,
                    "existing partition table differs; stop before writing")
            result["preflight"] = "PASS"
            for part in firmware.components:
                require((output / part.name).read_bytes() == part.data, "prepared component changed while waiting")
            result.update(write="IN PROGRESS", write_attempts=1)
            report("write")  # Persist intent before the single write command.
            wrote = call("write-flash", ["--no-progress", "--flash-mode", "dio", "--flash-freq", "80m",
                                         "--flash-size", "8MB", *paths], no_stub=False)
            require(wrote.count("Hash of data verified.") == len(firmware.components), "write verification was incomplete")
            result["write"] = "PASS"
            result["verify"] = "IN PROGRESS"
            report("verify")
            verified = call("verify-flash", ["--flash-mode", "keep", "--flash-freq", "keep",
                                             "--flash-size", "keep", *paths], no_stub=False)
            require(verified.count("Verification successful (digest matched)") == len(firmware.components),
                    "independent component verification was incomplete")
            result["verify"] = "PASS"
            result["boot"] = "IN PROGRESS"
            report("boot")
            call("run", before="usb-reset", after="hard-reset", no_stub=False)
            lines, closed = backend.observe(identity, options.boot_seconds)
            evidence, text = boot_evidence(lines, firmware.version)
            result.update(evidence, serial_closed=closed)
            (output / "startup-sanitized.log").write_text(text)
            report("completed" if result["boot"] == "PASS" else "stopped")
            return result
    except (Exception, KeyboardInterrupt) as error:
        if result["write"] == "IN PROGRESS":
            result["write"] = "UNCERTAIN"
        for key in ("preflight", "verify", "boot"):
            if result[key] == "IN PROGRESS":
                result[key] = "FAIL"
        result["error"] = clean_output(str(error)) or type(error).__name__
        result["serial_closed"] = getattr(backend, "serial_closed", True)
        result["manual_review_required"] = result["write_attempts"] > 0
        if getattr(backend, "last_output", None):
            (output / "last-command-sanitized.log").write_text(clean_output(backend.last_output) + "\n")
        report("stopped")
        return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path, help="extracted CI directory or merged full.bin")
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expected-version")
    parser.add_argument("--execute", action="store_true", help="explicit permission to reset and flash this target")
    parser.add_argument("--target-serial", help="known native USB serial; never select the first arbitrary port")
    parser.add_argument("--output", type=Path, help="new private receipt directory; must not already exist")
    parser.add_argument("--wait-seconds", type=float, default=600)
    parser.add_argument("--boot-seconds", type=float, default=25)
    parser.add_argument("--command-seconds", type=float, default=120)
    args = parser.parse_args(argv)
    try:
        firmware = verify_package(args.artifact, args.sha256, args.manifest, args.expected_version)
        if not args.execute:
            print(json.dumps({"mode": "dry-run", "device_opened": False, **firmware.summary()}, indent=2))
            return 0
        require(bool(args.target_serial) and args.output is not None, "--execute requires --target-serial and --output")
        options = Options(serial_identity(args.target_serial), args.wait_seconds, args.boot_seconds, args.command_seconds)
        backend = EsptoolBackend()
        result = execute(firmware, options, args.output, backend)
        return 0 if result["write"] == result["verify"] == result["boot"] == "PASS" else 1
    except (ValueError, OSError, RuntimeError) as error:
        print("Stopped: " + clean_output(str(error)), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
