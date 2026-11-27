#!/usr/bin/env python3
"""Assemble a self-contained Arduino IDE & PlatformIO library bundle (.zip or folder).

Because Arduino IDE forbids relative '../' includes outside a library directory
and PlatformIO remote URL installs expect library.json at the archive root,
this script packages the single-source cleanroom/ C99 core together with esp32/
HAL sources, library.properties, and a standalone library.json into a single
archive without duplicating source files in git.

Usage:
    python3 esp32/tools/make_arduino_bundle.py --out dist/openlnk-arduino.zip
"""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
import zipfile
from pathlib import Path

CORE_C_FILES = (
    "openlnk.c",
    "crc16.c",
    "protocol.c",
    "storage_mem.c",
    "server.c",
    "client.c",
)

CORE_H_FILES = (
    "client.h",
    "crc16.h",
    "openlnk.h",
    "protocol.h",
    "server.h",
    "storage.h",
    "version.h",
)

ESP32_C_FILES = (
    "esp32_hal_uart.c",
    "esp32_hal_sd.c",
    "esp32_hal_img.c",
    "esp32_server.c",
    "esp32_client.c",
    "esp32_client_vfs.c",
)


def make_standalone_library_json(src_manifest: Path) -> str:
    """Adapt esp32/library.json for the self-contained bundle layout."""
    data = json.loads(src_manifest.read_text(encoding="utf-8"))
    data["build"] = {
        "srcDir": "src",
        "includeDir": "src",
        "srcFilter": [
            "+<*.c>",
            "+<openlnk_core/*.c>",
        ],
        "extraScript": "pio_build.py",
    }
    return json.dumps(data, indent=2) + "\n"


def build_bundle(repo_root: Path, out_path: Path) -> Path:
    esp32_dir = repo_root / "esp32"
    cleanroom_dir = repo_root / "cleanroom"

    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "openlnk"
        src_dir = stage / "src"
        core_src_dir = src_dir / "openlnk_core"
        core_inc_dir = src_dir / "openlnk"
        ex_dir = stage / "examples" / "sd_server_arduino"

        core_src_dir.mkdir(parents=True)
        core_inc_dir.mkdir(parents=True)
        ex_dir.mkdir(parents=True)

        # Arduino + PlatformIO manifests & docs
        shutil.copy2(esp32_dir / "library.properties", stage / "library.properties")
        (stage / "library.json").write_text(
            make_standalone_library_json(esp32_dir / "library.json"),
            encoding="utf-8",
        )
        shutil.copy2(esp32_dir / "pio_build.py", stage / "pio_build.py")
        if (esp32_dir / "README.md").is_file():
            shutil.copy2(esp32_dir / "README.md", stage / "README.md")
        if (repo_root / "LICENSE").is_file():
            shutil.copy2(repo_root / "LICENSE", stage / "LICENSE")

        # Public ESP32 header + HAL sources
        shutil.copy2(esp32_dir / "include" / "openlnk_esp32.h", src_dir / "openlnk_esp32.h")
        for fname in ESP32_C_FILES:
            shutil.copy2(esp32_dir / "src" / fname, src_dir / fname)

        # Single-source cleanroom C99 headers & implementation
        for fname in CORE_H_FILES:
            shutil.copy2(cleanroom_dir / "include" / "openlnk" / fname, core_inc_dir / fname)
        for fname in CORE_C_FILES:
            shutil.copy2(cleanroom_dir / "src" / fname, core_src_dir / fname)

        # Example sketch (.ino)
        arduino_main = esp32_dir / "examples" / "sd_server_arduino" / "src" / "main.cpp"
        if arduino_main.is_file():
            shutil.copy2(arduino_main, ex_dir / "sd_server_arduino.ino")

        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.suffix.lower() == ".zip":
            with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                for item in sorted(stage.rglob("*")):
                    if item.is_file():
                        zf.write(item, item.relative_to(stage.parent).as_posix())
        else:
            if out_path.exists():
                shutil.rmtree(out_path)
            shutil.copytree(stage, out_path)

    return out_path


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    parser = argparse.ArgumentParser(
        description="Build self-contained Arduino IDE & PlatformIO bundle for OpenLNK"
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=repo_root / "dist" / "openlnk-arduino.zip",
        help="Output .zip file or directory path (default: dist/openlnk-arduino.zip)",
    )
    args = parser.parse_args()
    result = build_bundle(repo_root, args.out)
    print(f"[openlnk] ESP32 Arduino/PlatformIO bundle created: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
