#!/usr/bin/env python3
"""Synthetic CI bytes and mocked serial/backend checks; never touches a device."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import flash_ci_firmware as FLASH
import verify_ci_firmware as VERIFY

IDENTITY = "001122334455"  # Synthetic, not a recorded board ID.
PORT = SimpleNamespace(device="/dev/mock-c3", vid=0x303A, pid=0x1001, serial_number="00:11:22:33:44:55")
SECURITY = "ESP32-C3\nMAC: 00:11:22:33:44:55\nFlags: 0x00000000\nChip ID: 5\nSecure Boot: Disabled\nFlash Encryption: Disabled\n"


def esp_image(payload, address):
    image = bytearray(24)
    image[:4] = bytes((0xE9, 1, 2, 0x3F))
    struct.pack_into("<H", image, 12, 5)
    image[23] = 1
    image += struct.pack("<II", address, len(payload)) + payload
    checksum = 0xEF
    for value in payload: checksum ^= value
    image += bytes(15 - len(image) % 16) + bytes((checksum,))
    return bytes(image) + hashlib.sha256(image).digest()


class Clock:
    def __init__(self): self.value = 0
    def now(self): return self.value
    def sleep(self, seconds): self.value += seconds


class FakeBackend:
    def __init__(self, firmware, ports=None, fail=None):
        self.firmware, self.available = firmware, [PORT] if ports is None else ports
        self.fail, self.calls, self.serial_closed, self.last_output = fail, [], True, ""
        self.security = SECURITY
        self.capacity = "Detected flash size: 8MB"
        self.bad_table = False
        self.boot_lines = [f"I (1) app_init: App version: {firmware.version}",
            "I (2) main_task: Calling app_main()", "I (3) bsp_disp: 显示就绪 240x320",
            "I (4) bsp_lvgl: LVGL 就绪", "I (5) bsp_btn: 按键就绪"]

    def ports(self): return self.available
    def check_owner(self, port): pass
    def call(self, port, name, arguments, before, after, no_stub, timeout):
        self.calls.append((name, arguments, before, after, no_stub))
        if name == self.fail: raise RuntimeError("simulated command failure")
        if name == "get-security-info": return self.security
        if name == "flash-id": return self.capacity
        if name == "read-flash":
            table = next(p.data for p in self.firmware.components if p.offset == 0x8000)
            Path(arguments[-1]).write_bytes((b"\x00" * len(table) if self.bad_table else table) + b"\xff" * (4096-len(table)))
            return "read complete"
        if name == "write-flash": return "Hash of data verified.\n" * 3
        if name == "verify-flash": return "Verification successful (digest matched).\n" * 3
        if name == "run": return "Hard resetting"
        raise AssertionError(name)
    def observe(self, identity, seconds): return self.boot_lines, True


class FlashToolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="passport-tools-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.artifact = self.root / "artifact"
        self.artifact.mkdir()
        self.version = "1111111"
        boot = esp_image(bytes(96), 0x3FC80000)
        descriptor = bytearray(256)
        struct.pack_into("<I", descriptor, 0, 0xABCD5432)
        descriptor[16:48] = self.version.encode().ljust(32, b"\0")
        descriptor[48:80] = VERIFY.APP.encode().ljust(32, b"\0")
        descriptor[112:144] = b"v5.5.3-dirty".ljust(32, b"\0")
        descriptor[144:176] = hashlib.sha256(b"synthetic ELF").digest()
        app = esp_image(descriptor, 0x3C000020)
        table = bytearray(b"\xff" * 0xC00)
        for i, (label, kind, subtype, offset, size) in enumerate(VERIFY.LAYOUT):
            struct.pack_into("<HBBII16sI", table, i*32, 0x50AA, kind, subtype, offset, size,
                             label.encode().ljust(16, b"\0"), 0)
        struct.pack_into("<H", table, 96, 0xEBEB)
        table[112:128] = hashlib.md5(table[:96]).digest()
        self.components = {"bootloader/bootloader.bin": bytes(boot),
                           "partition_table/partition-table.bin": bytes(table), f"{VERIFY.APP}.bin": bytes(app)}
        raw = bytearray(b"\xff" * (0x10000+len(app)))
        for name, offset in VERIFY.OFFSETS.items(): raw[offset:offset+len(self.components[name])] = self.components[name]
        self.raw = bytes(raw)
        self.digest = VERIFY.sha256(self.raw)
        (self.artifact / VERIFY.FULL_BIN).write_bytes(self.raw)
        (self.artifact / "SHA256SUMS").write_text(f"{self.digest}  {VERIFY.FULL_BIN}\n")
        files = {name: {"size":len(data), "sha256":VERIFY.sha256(data)} for name,data in self.components.items()}
        files[VERIFY.FULL_BIN] = {"size":len(self.raw), "sha256":self.digest}
        descriptor = VERIFY.application_descriptor(bytes(app))
        files[f"{VERIFY.APP}.elf"] = {"size":13,"sha256":descriptor["embedded_elf_sha256"]}
        self.manifest = {"target":"esp32c3","flash_size":"8MB","flash_offset":"0x0","idf_version":"5.5.3",
            "source_commit":"1"*40,"github_run_id":"123","merged_image":{"name":VERIFY.FULL_BIN,**files[VERIFY.FULL_BIN]},
            "validated_build_manifest":{"schema_version":1,"target":"esp32c3","flash_size_bytes":8*1024*1024,
                "image_offsets":dict(VERIFY.OFFSETS),"full_bin_sha256":self.digest,"files":files,
                "app_descriptor":descriptor,"app_elf_sha256":descriptor["embedded_elf_sha256"]}}
        self.write_manifest()
        self.firmware = VERIFY.verify_package(self.artifact, self.digest)
        self.clock = Clock()

    def write_manifest(self):
        (self.artifact / "manifest.json").write_text(json.dumps(self.manifest))

    def run_flash(self, backend, output=None):
        with patch.object(FLASH, "target_lock", lambda identity: contextlib.nullcontext()):
            return FLASH.execute(self.firmware, FLASH.Options(IDENTITY, 1, 1, 1),
                output or self.root / "run", backend, self.clock.now, self.clock.sleep, lambda _: None)

    def test_valid_package_and_protected_erase_ranges(self):
        self.assertEqual(self.firmware.version, self.version)
        self.assertEqual([p.offset for p in self.firmware.components], [0,0x8000,0x10000])
        for p in self.firmware.components:
            self.assertTrue(p.erase_end <= 0x9000 or p.erase_start >= 0x10000)

    def test_hash_mismatch(self):
        with self.assertRaisesRegex(ValueError, "approved SHA256"):
            VERIFY.verify_package(self.artifact, "0"*64)

    def test_component_hash_mismatch(self):
        self.manifest["validated_build_manifest"]["files"][f"{VERIFY.APP}.bin"]["sha256"]="0"*64
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            VERIFY.verify_package(self.artifact,self.digest)

    def test_manifest_cannot_choose_truncated_application(self):
        size = len(self.components[f"{VERIFY.APP}.bin"]) - 16
        self.manifest["validated_build_manifest"]["files"][f"{VERIFY.APP}.bin"] = {
            "size": size, "sha256": VERIFY.sha256(self.raw[0x10000:0x10000 + size])}
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "merged image end"):
            VERIFY.verify_package(self.artifact, self.digest)

    def test_manifest_cannot_shorten_bootloader(self):
        size = len(self.components["bootloader/bootloader.bin"]) - 16
        self.manifest["validated_build_manifest"]["files"]["bootloader/bootloader.bin"] = {
            "size": size, "sha256": VERIFY.sha256(self.raw[:size])}
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "gap contains data"):
            VERIFY.verify_package(self.artifact, self.digest)

    def test_complete_image_checksum_digest_and_length(self):
        data = self.components[f"{VERIFY.APP}.bin"]
        for corrupted in (data[:-1], data + b"\xff", data[:48] + b"\x42" + data[49:],
                          data[:-1] + bytes((data[-1] ^ 1,))):
            with self.subTest(length=len(corrupted)), self.assertRaises(ValueError):
                VERIFY.verify_esp_image(corrupted)

    def test_invalid_offset_mapping(self):
        self.manifest["validated_build_manifest"]["image_offsets"][f"{VERIFY.APP}.bin"]=0x9000
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "mapping"):
            VERIFY.verify_package(self.artifact,self.digest)

    def test_sector_erasure_into_nvs_rejected(self):
        build=self.manifest["validated_build_manifest"]
        build["files"]["partition_table/partition-table.bin"]={"size":0x1001,"sha256":VERIFY.sha256(self.raw[0x8000:0x9001])}
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "NVS or PHY"):
            VERIFY.verify_package(self.artifact,self.digest)

    def test_expected_version_mismatch(self):
        with self.assertRaisesRegex(ValueError, "version mismatch"):
            VERIFY.verify_package(self.artifact,self.digest,expected_version="different")

    def test_duplicate_manifest_key_rejected(self):
        p=self.artifact/"manifest.json"
        p.write_text('{"target":"esp32c3",'+p.read_text()[1:])
        with self.assertRaises(ValueError): VERIFY.verify_package(self.artifact,self.digest)

    def test_symlink_input_rejected(self):
        image=self.artifact/VERIFY.FULL_BIN
        real=self.root/"payload.bin";image.rename(real);image.symlink_to(real)
        with self.assertRaisesRegex(ValueError,"symlink"):
            VERIFY.verify_package(self.artifact,self.digest)

    def test_dry_run_never_loads_hardware_backend(self):
        with patch.object(FLASH,"EsptoolBackend",side_effect=AssertionError("hardware accessed")), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(FLASH.main([str(self.artifact),"--sha256",self.digest]),0)
        self.assertEqual(set(p.name for p in self.artifact.iterdir()), {VERIFY.FULL_BIN,"manifest.json","SHA256SUMS"})

    def test_execute_requires_explicit_target_and_receipt(self):
        with patch.object(FLASH,"EsptoolBackend") as backend, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(FLASH.main([str(self.artifact),"--sha256",self.digest,"--execute"]),1)
        backend.assert_not_called()

    def test_optimized_python_still_enforces_checks(self):
        process=subprocess.run([sys.executable,"-O",str(ROOT/"tools/verify_ci_firmware.py"),str(self.artifact),"--sha256","0"*64],capture_output=True,text=True)
        self.assertNotEqual(process.returncode,0)

    def test_no_device_expires_without_commands(self):
        backend=FakeBackend(self.firmware,ports=[])
        result=self.run_flash(backend)
        self.assertEqual(backend.calls,[])
        self.assertEqual(result["write_attempts"],0)
        self.assertEqual(result["phase"],"stopped")
        self.assertEqual(result["write"],"NOT RUN")
        self.assertLessEqual(self.clock.value,1.01)

    def test_multiple_matching_devices_stop(self):
        backend=FakeBackend(self.firmware,ports=[PORT,SimpleNamespace(**{**vars(PORT),"device":"/dev/mock-other"})])
        result=self.run_flash(backend)
        self.assertEqual(backend.calls,[])
        self.assertIn("multiple",result["error"])

    def test_wrong_usb_identity_is_not_selected(self):
        other=SimpleNamespace(**{**vars(PORT),"serial_number":"66:77:88:99:AA:BB"})
        self.assertIsNone(FLASH.select_target([other],IDENTITY))

    def test_wrong_chip_and_security_stop_before_write(self):
        for text in [SECURITY.replace("Chip ID: 5","Chip ID: 9"),SECURITY.replace("0x00000000","0x00000001"),SECURITY.replace("Encryption: Disabled","Encryption: Enabled")]:
            with self.subTest(text=text):
                backend=FakeBackend(self.firmware);backend.security=text
                result=self.run_flash(backend,self.root/str(len(list(self.root.iterdir()))))
                self.assertEqual(result["write_attempts"],0)
                self.assertEqual([c[0] for c in backend.calls],["get-security-info"])

    def test_wrong_capacity_stops_before_write(self):
        backend=FakeBackend(self.firmware);backend.capacity="Detected flash size: 4MB"
        result=self.run_flash(backend)
        self.assertEqual(result["write_attempts"],0)

    def test_partition_mismatch_stops_before_write(self):
        backend=FakeBackend(self.firmware);backend.bad_table=True
        result=self.run_flash(backend)
        self.assertEqual(result["write_attempts"],0)
        self.assertEqual([c[0] for c in backend.calls],["get-security-info","flash-id","read-flash"])

    def test_write_failure_is_uncertain_and_no_verify_or_retry(self):
        backend=FakeBackend(self.firmware,fail="write-flash")
        result=self.run_flash(backend)
        names=[c[0] for c in backend.calls]
        self.assertEqual(names.count("write-flash"),1)
        self.assertNotIn("verify-flash",names)
        self.assertNotIn("run",names)
        self.assertEqual(result["write"],"UNCERTAIN")
        self.assertTrue(result["manual_review_required"])
        with self.assertRaises(FileExistsError): self.run_flash(backend)
        self.assertEqual([c[0] for c in backend.calls].count("write-flash"),1)

    def test_verify_failure_does_not_boot_or_rewrite(self):
        backend=FakeBackend(self.firmware,fail="verify-flash")
        result=self.run_flash(backend)
        self.assertEqual(result["write"],"PASS")
        self.assertEqual(result["verify"],"FAIL")
        self.assertNotIn("run",[c[0] for c in backend.calls])
        self.assertEqual([c[0] for c in backend.calls].count("write-flash"),1)

    def test_success_flow_writes_exact_offsets_once_and_keeps_bytes(self):
        backend=FakeBackend(self.firmware)
        result=self.run_flash(backend)
        self.assertEqual((result["write"],result["verify"],result["boot"]),("PASS",)*3)
        self.assertEqual(result["write_attempts"],1)
        self.assertFalse(result["nvs_written"] or result["phy_written"] or result["whole_chip_erase"])
        calls={c[0]:c for c in backend.calls}
        self.assertEqual(calls["read-flash"][1][:2],["0x8000","0x1000"])
        write_args=calls["write-flash"][1]
        self.assertEqual(write_args[7::2],["0x0","0x8000","0x10000"])
        self.assertNotIn("--erase-all",write_args)
        self.assertNotIn("--force",write_args)
        self.assertEqual(calls["verify-flash"][1],
                         ["--flash-mode","keep","--flash-freq","keep","--flash-size","keep",*write_args[7:]])
        for name in ("read-flash", "write-flash", "verify-flash"):
            self.assertEqual(calls[name][3], "no-reset-stub")
        self.assertTrue(result["display_ready"] and result["lvgl_ready"] and result["buttons_ready"])

    def test_boot_wrong_version_or_crash_fails_without_reflash(self):
        for lines in [["main_task: Calling app_main()"],[f"app_init: App version: {self.version}","main_task: Calling app_main()","Guru Meditation Error password=example"]]:
            backend=FakeBackend(self.firmware);backend.boot_lines=lines
            result=self.run_flash(backend,self.root/str(len(list(self.root.iterdir()))))
            self.assertEqual(result["boot"],"FAIL")
            self.assertEqual([c[0] for c in backend.calls].count("write-flash"),1)
            if len(lines)==3:
                self.assertTrue(result["crash_observed"])
                self.assertNotIn("password",(self.root/str(len(list(self.root.iterdir()))-1)/"startup-sanitized.log").read_text())

    def test_log_redaction(self):
        self.assertIsNone(FLASH.sanitize("reset_wifi: ssid=private password=example"))
        self.assertIsNone(FLASH.sanitize("boot: token=example"))
        self.assertIsNone(FLASH.sanitize("boot: https://example.test/?key=example"))
        self.assertEqual(FLASH.sanitize("MAC: 00:11:22:33:44:55 IP 192.0.2.1"),"MAC: [MAC redacted] IP [IP redacted]")

    def test_control_characters_cannot_hide_sensitive_markers_or_crashes(self):
        for line in ("boot: pass\x00word=example", "boot: to\x08ken=example",
                     "boot: htt\x00ps://example.test/"):
            self.assertIsNone(FLASH.sanitize(line))
        evidence, saved = FLASH.boot_evidence([f"app_init: App version: {self.version}",
            "main_task: Calling app_main()", "Guru Medi\x00tation pass\x00word=example"], self.version)
        self.assertEqual(evidence["boot"], "FAIL")
        self.assertTrue(evidence["crash_observed"])
        self.assertNotIn("example", saved)

    def test_child_disables_internal_write_and_block_retries(self):
        # Run the actual child code against a fake esptool module, without serial imports.
        package = self.root / "esptool"
        package.mkdir()
        (package / "loader.py").write_text("WRITE_BLOCK_ATTEMPTS = 3\nclass ESPLoader:\n    WRITE_FLASH_ATTEMPTS = 2\n")
        (package / "__init__.py").write_text("""__version__ = '5.4.0'
def main():
    from . import loader
    writes = 0
    for _ in range(loader.ESPLoader.WRITE_FLASH_ATTEMPTS):
        for _ in range(loader.WRITE_BLOCK_ATTEMPTS):
            writes += 1  # A simulated failure would ordinarily be retried.
    print(writes)
    raise SystemExit(1)
""")
        process = subprocess.run([sys.executable, "-c", FLASH.ESPTOOL_CHILD, "write-flash"],
            cwd=self.root, env={**os.environ, "PYTHONPATH":str(self.root), "PYTHONDONTWRITEBYTECODE":"1"},
            capture_output=True, text=True, timeout=5)
        self.assertEqual(process.returncode, 1)
        self.assertEqual(process.stdout.strip(), "1")

    def test_backend_child_ignores_inherited_esptool_defaults(self):
        backend = FLASH.EsptoolBackend.__new__(FLASH.EsptoolBackend)
        with patch.dict(os.environ, {"ESPTOOL_FM":"qio", "ESPTOOL_FF":"40m"}), \
             patch.object(FLASH.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="Verification successful (digest matched)", stderr="")) as launch:
            backend.call(PORT.device, "verify-flash", [], "no-reset", "no-reset-stub", False, 1)
        args, kwargs = launch.call_args
        self.assertEqual(args[0][:3], [sys.executable, "-c", FLASH.ESPTOOL_CHILD])
        self.assertFalse(any(key.startswith("ESPTOOL_") for key in kwargs["env"]))
        self.assertEqual(kwargs["timeout"], 1)

    def test_invalid_timeout_stops_before_any_command(self):
        for value in [0,float("nan"),float("inf"),3601]:
            with self.assertRaises(ValueError):
                FLASH.execute(self.firmware,FLASH.Options(IDENTITY,value),self.root/"invalid",FakeBackend(self.firmware))

    def test_target_lock_rejects_concurrent_run(self):
        with patch.object(FLASH.tempfile,"gettempdir",return_value=str(self.root)):
            with FLASH.target_lock(IDENTITY):
                with self.assertRaises(FileExistsError):
                    with FLASH.target_lock(IDENTITY): pass

    def test_serial_finally_closed_on_read_error(self):
        monitor=SimpleNamespace(is_open=False,dtr=False,rts=False,port=None)
        def opened(): monitor.is_open=True
        def closed(): monitor.is_open=False
        monitor.open=opened;monitor.close=closed
        def failed_read(size): raise OSError("simulated serial read failure")
        monitor.read=failed_read
        backend=FLASH.EsptoolBackend.__new__(FLASH.EsptoolBackend)
        backend.serial=SimpleNamespace(Serial=lambda **kwargs:monitor)
        backend.ports=lambda:[PORT];backend.check_owner=lambda _:None;backend.serial_closed=True
        with self.assertRaises(OSError): backend.observe(IDENTITY,0.01)
        self.assertFalse(monitor.is_open)
        self.assertTrue(backend.serial_closed)


if __name__ == "__main__":
    unittest.main()
