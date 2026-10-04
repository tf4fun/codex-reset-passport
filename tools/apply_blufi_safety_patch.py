#!/usr/bin/env python3
"""Apply/check the AI Passport-only BLUFI fragment bounds patch for IDF 5.5.3.

This is not a general upstream fix: it caps reassembled messages at 1025 bytes,
matching this application's largest supported DH negotiation message. It does
not change BLUFI authentication, encryption policy, or the provisioning UI.
Unknown SDK versions and modified source files are refused without writing.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PATCH_BASE = ROOT / "tools/patches/esp-idf-5.5.3-blufi-fragment-bounds"
MANIFEST = json.loads(Path(str(PATCH_BASE) + ".json").read_text(encoding="utf-8"))
PATCH = Path(str(PATCH_BASE) + ".patch").read_text(encoding="utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def apply_unified(source: str, patch: str, *, line_offset: int = 0) -> str:
    """Apply exact unified hunks in memory, requiring every context line to match.

    line_offset supports the pinned function-only host-test fixture. Production
    application always patches the complete file with the default zero offset.
    """
    original = source.splitlines(keepends=True)
    lines = patch.splitlines(keepends=True)
    if len(lines) < 3 or not lines[0].startswith("--- ") or not lines[1].startswith("+++ "):
        raise ValueError("Invalid patch header")
    output: list[str] = []
    cursor = 0
    index = 2
    while index < len(lines):
        match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n", lines[index])
        if not match:
            raise ValueError("Invalid patch hunk")
        start = int(match[1]) - 1 - line_offset
        old_count = int(match[2] or "1")
        new_count = int(match[4] or "1")
        if start < cursor or start > len(original):
            raise ValueError("Patch hunk outside source")
        output.extend(original[cursor:start])
        cursor = start
        consumed = emitted = 0
        index += 1
        while index < len(lines) and not lines[index].startswith("@@ "):
            line = lines[index]
            if not line or line[0] not in " +-":
                raise ValueError("Unsupported patch line")
            if line[0] in " -":
                if cursor >= len(original) or original[cursor] != line[1:]:
                    raise ValueError("Patch context mismatch")
                cursor += 1
                consumed += 1
            if line[0] in " +":
                output.append(line[1:])
                emitted += 1
            index += 1
        if (consumed, emitted) != (old_count, new_count):
            raise ValueError("Patch hunk count mismatch")
    output.extend(original[cursor:])
    return "".join(output)


def sdk_version(root: Path) -> str:
    text = (root / "tools/cmake/version.cmake").read_text(encoding="utf-8")
    parts = []
    for component in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^set\(IDF_VERSION_{component} (\d+)\)$", text, re.MULTILINE)
        if not match:
            raise ValueError("Unrecognized SDK version file")
        parts.append(match[1])
    return ".".join(parts)


def process(root: Path, *, apply: bool) -> str:
    if sdk_version(root) != MANIFEST["idf_version"]:
        raise ValueError("Only the pinned ESP-IDF 5.5.3 source is supported")
    path = root / MANIFEST["source_path"]
    original = path.read_bytes()
    current_hash = digest(original)
    if current_hash == MANIFEST["patched_sha256"]:
        return "PASS: AI Passport BLUFI fragment bounds patch verified"
    if current_hash != MANIFEST["upstream_sha256"]:
        raise ValueError("BLUFI source hash is unknown; refusing to overwrite SDK changes")
    if not apply:
        raise ValueError("BLUFI bounds patch is missing; explicitly run this tool with --apply")
    patched = apply_unified(original.decode("utf-8"), PATCH).encode("utf-8")
    if digest(patched) != MANIFEST["patched_sha256"]:
        raise ValueError("Patch result hash mismatch; SDK was not changed")
    # All checks finish before the only write, which is an atomic replacement.
    temporary = path.with_name(path.name + ".passport-patch.tmp")
    if temporary.exists():
        raise ValueError("Temporary patch file already exists; inspect it before retrying")
    try:
        with temporary.open("xb") as stream:
            stream.write(patched)
        temporary.chmod(path.stat().st_mode)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return "PASS: applied AI Passport-only BLUFI fragment bounds patch"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--apply", action="store_true", help="explicitly patch the known original SDK file")
    mode.add_argument("--check", action="store_true", help="read-only: require the exact already-patched SDK file")
    args = parser.parse_args()
    if args.idf_path is None:
        parser.error("set IDF_PATH or pass --idf-path")
    try:
        print(process(Path(args.idf_path).resolve(), apply=args.apply))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"BLUFI safety check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
