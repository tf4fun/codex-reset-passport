#!/usr/bin/env python3
"""Offline verification and component extraction plan for a CI firmware package."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import stat
import struct
import sys

sys.dont_write_bytecode = True
from archive_firmware import APP, FULL_BIN, application_descriptor, safe_path, unique_json_object
from verify_firmware import FLASH_SIZE, PARTITION_TABLE_SIZE, parse_partition_table


OFFSETS = {
    "bootloader/bootloader.bin": 0,
    "partition_table/partition-table.bin": 0x8000,
    f"{APP}.bin": 0x10000,
}
LAYOUT = (
    ("nvs", 1, 2, 0x9000, 0x6000),
    ("phy_init", 1, 1, 0xF000, 0x1000),
    ("factory", 0, 0, 0x10000, 0x7F0000),
)
PROTECTED = ((0x9000, 0x10000),)  # NVS and PHY, including sector boundaries.
HASH = re.compile(r"[0-9a-f]{64}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_regular(path: Path, limit: int) -> bytes:
    path = safe_path(path)
    require(stat.S_ISREG(path.stat().st_mode), "input must be a regular file")
    require(0 < path.stat().st_size <= limit, "input size is outside its allowed bounds")
    with path.open("rb") as handle:
        data = handle.read(limit + 1)
    require(0 < len(data) <= limit, "input grew beyond its allowed bounds")
    return data


def file_entry(entry: object, data: bytes) -> None:
    require(isinstance(entry, dict), "missing image checksum record")
    require(type(entry.get("size")) is int and entry["size"] == len(data), "image size mismatch")
    require(isinstance(entry.get("sha256"), str) and HASH.fullmatch(entry["sha256"]) is not None,
            "invalid image checksum record")
    require(entry["sha256"] == sha256(data), "image SHA256 mismatch")


def verify_esp_image(data: bytes) -> None:
    """Validate the complete unsigned IDF C3 image, not a manifest-chosen prefix."""
    require(len(data) >= 24 and data[0] == 0xE9 and 1 <= data[1] <= 16
            and struct.unpack_from("<H", data, 12)[0] == 5, "image is not ESP32-C3")
    require(data[23] == 1, "supported CI images must append their SHA256 digest")
    cursor, checksum = 24, 0xEF
    for _ in range(data[1]):
        require(cursor + 8 <= len(data), "truncated ESP image segment header")
        _, size = struct.unpack_from("<II", data, cursor)
        cursor += 8
        require(size % 4 == 0 and cursor + size <= len(data), "truncated or unaligned ESP image segment")
        for value in data[cursor:cursor + size]:
            checksum ^= value
        cursor += size
    checksum_offset = cursor // 16 * 16 + 15
    end = checksum_offset + 1
    require(end + 32 == len(data), "ESP image component length differs from its complete image")
    require(data[checksum_offset] == checksum, "ESP image checksum mismatch")
    require(hashlib.sha256(data[:end]).digest() == data[end:], "ESP image appended SHA256 mismatch")


@dataclass(frozen=True)
class Component:
    name: str
    offset: int
    data: bytes

    @property
    def erase_start(self) -> int:
        return self.offset // 4096 * 4096

    @property
    def erase_end(self) -> int:
        return (self.offset + len(self.data) + 4095) // 4096 * 4096

    def summary(self) -> dict:
        return {"name": self.name, "offset": hex(self.offset), "size": len(self.data),
                "sha256": sha256(self.data), "erase_start": hex(self.erase_start),
                "erase_end_exclusive": hex(self.erase_end)}


@dataclass(frozen=True)
class Firmware:
    digest: str
    source_commit: str
    run_id: str
    version: str
    components: tuple[Component, ...]

    def summary(self) -> dict:
        return {"sha256": self.digest, "source_commit": self.source_commit,
                "github_run_id": self.run_id, "expected_version": self.version,
                "target": "esp32c3", "flash_size": "8MB", "flash_mode": "dio",
                "flash_frequency": "80m", "components": [part.summary() for part in self.components],
                "preserved_ranges": ["0x9000..0xefff NVS", "0xf000..0xffff PHY"]}


def verify_package(artifact: Path, expected_sha256: str, manifest_path: Path | None = None,
                   expected_version: str | None = None) -> Firmware:
    require(HASH.fullmatch(expected_sha256) is not None, "expected SHA256 must be 64 lowercase hex digits")
    artifact = safe_path(artifact)
    image = artifact / FULL_BIN if artifact.is_dir() else artifact
    directory = image.parent
    manifest_path = safe_path(manifest_path or directory / "manifest.json")
    raw = read_regular(image, FLASH_SIZE)
    require(sha256(raw) == expected_sha256, "merged firmware differs from the approved SHA256")
    manifest = json.loads(read_regular(manifest_path, 128 * 1024), object_pairs_hook=unique_json_object)
    require(isinstance(manifest, dict), "invalid CI manifest")
    try:
        require(manifest["target"] == "esp32c3" and manifest["flash_size"] == "8MB"
                and manifest["flash_offset"] == "0x0" and manifest["idf_version"] == "5.5.3",
                "unsupported CI target, flash configuration or SDK")
        source = manifest["source_commit"]
        run_id = manifest["github_run_id"]
        require(isinstance(source, str) and re.fullmatch(r"[0-9a-f]{40}", source) is not None,
                "invalid source commit")
        require(isinstance(run_id, str) and re.fullmatch(r"[0-9]{1,24}", run_id) is not None,
                "invalid CI run identity")
        require(manifest["merged_image"]["name"] == FULL_BIN, "unexpected merged image name")
        file_entry(manifest["merged_image"], raw)
        sums = read_regular(directory / "SHA256SUMS", 512)
        require(sums == f"{expected_sha256}  {FULL_BIN}\n".encode(), "SHA256SUMS mismatch")
        build = manifest["validated_build_manifest"]
        require(build["schema_version"] == 1 and build["target"] == "esp32c3"
                and build["flash_size_bytes"] == FLASH_SIZE, "unsupported build manifest")
        require(build["image_offsets"] == OFFSETS, "component mapping must match the supported layout")
        require(build["full_bin_sha256"] == expected_sha256, "build manifest digest mismatch")
        file_entry(build["files"][FULL_BIN], raw)
        components = []
        previous_end = 0
        for name, offset in sorted(OFFSETS.items(), key=lambda item: item[1]):
            entry = build["files"][name]
            require(type(entry["size"]) is int and entry["size"] > 0, "invalid component size")
            end = offset + entry["size"]
            require(previous_end <= offset < end <= len(raw), "component bounds overlap or exceed the image")
            require(raw[previous_end:offset] == b"\xff" * (offset - previous_end),
                    "merged gap contains data outside the verified components")
            part = Component(name, offset, raw[offset:end])
            file_entry(entry, part.data)
            require(all(part.erase_end <= start or part.erase_start >= stop for start, stop in PROTECTED),
                    "component erasure would touch NVS or PHY")
            components.append(part)
            previous_end = end
        boot, table, app = components
        require(previous_end == len(raw), "application must reach the merged image end")
        verify_esp_image(boot.data)
        verify_esp_image(app.data)
        require(boot.data[2:4] == bytes((2, 0x3F)), "bootloader must use DIO, 80MHz and 8MB")
        require(len(table.data) == PARTITION_TABLE_SIZE, "unexpected partition table size")
        partitions, md5 = parse_partition_table(table.data)
        require(md5 and tuple((p.label, p.kind, p.subtype, p.offset, p.size) for p in partitions) == LAYOUT,
                "only the documented minimal partition layout is supported by this flash tool")
        require(len(app.data) <= LAYOUT[-1][-1], "application exceeds its factory partition")
        descriptor = application_descriptor(app.data)
        require(descriptor == build["app_descriptor"], "binary application descriptor mismatch")
        require(descriptor["project_name"] == APP and descriptor["idf_version"] in ("v5.5.3", "v5.5.3-dirty"),
                "unexpected application or SDK identity")
        require(descriptor["embedded_elf_sha256"] == build["app_elf_sha256"]
                == build["files"][f"{APP}.elf"]["sha256"], "matching ELF identity mismatch")
        version = descriptor["version"]
        require(re.fullmatch(r"[A-Za-z0-9_.+\-]{1,31}", version) is not None, "invalid application version")
        require(expected_version is None or version == expected_version, "approved version mismatch")
        return Firmware(expected_sha256, source, run_id, version, tuple(components))
    except (KeyError, TypeError, IndexError) as error:
        raise ValueError("malformed CI firmware manifest") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path, help="extracted CI directory or merged full.bin")
    parser.add_argument("--sha256", required=True, help="approved checksum from the trusted CI handoff")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expected-version")
    args = parser.parse_args(argv)
    try:
        firmware = verify_package(args.artifact, args.sha256, args.manifest, args.expected_version)
        print(json.dumps({"verification": "PASS", **firmware.summary()}, indent=2))
        return 0
    except (ValueError, OSError) as error:
        print(f"Verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
