#!/usr/bin/env python3
"""Package only the verified current merged image and its provenance for CI."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
image = ROOT / "build/FoloToy-AI-Passport-full.bin"
digest = hashlib.sha256(image.read_bytes()).hexdigest()
archive = ROOT / "build/firmware" / digest
subprocess.run(["python3", str(ROOT / "tools/archive_firmware.py"), "verify", str(archive)], check=True)
manifest = json.loads((archive / "manifest.json").read_text())
out = ROOT / "build/ci-artifact"
out.mkdir(parents=True, exist_ok=False)
shutil.copyfile(image, out / image.name)
(out / "SHA256SUMS").write_text(f"{digest}  {image.name}\n")
(out / "manifest.json").write_text(json.dumps({
    "application": "Codex Reset v2.4", "target": "esp32c3", "flash_size": "8MB",
    "flash_offset": "0x0", "idf_version": "5.5.3",
    "source_commit": os.environ.get("GITHUB_SHA", subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()),
    "github_run_id": os.environ.get("GITHUB_RUN_ID"),
    "merged_image": {"name": image.name, "sha256": digest, "size": image.stat().st_size},
    "blufi_patch": json.loads((ROOT / "tools/patches/esp-idf-5.5.3-blufi-fragment-bounds.json").read_text()),
    "validated_build_manifest": manifest,
    "checks": {"static_and_host": "passed before packaging in CI", "firmware_and_merge": "passed before packaging in CI", "device": "not run"}
}, indent=2) + "\n")
(out / "FLASHING.txt").write_text("Codex Reset v2.4; ESP32-C3, 8MB flash.\n"
    "Verify SHA256SUMS before flashing the merged full.bin at offset 0x0.\n"
    "Merged flashing can reset stored NVS settings/Wi-Fi credentials.\n"
    "Do not use full-chip erase. Flash only the device and image authorized by its owner.\n"
    "CI checks compilation and software contracts, not physical device behavior.\n"
    "manifest.json identifies matching debug artifacts retained only on the build runner.\n")
print(f"CI artifact: {out}; SHA256 {digest}")
