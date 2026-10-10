#!/usr/bin/env python3
"""Create target-specific ClimaCarta OTA assets from both build directories."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path


VARIANTS = {
    "esp32c6": {
        "board": "waveshare-esp32-c6-epaper-1.54",
        "build": "build-c6",
        "binary": "climacarta-esp32c6.bin",
        "manifest": "manifest-esp32c6-epaper-1.54.json",
    },
    "esp32s3": {
        "board": "waveshare-esp32-s3-epaper-1.54g",
        "build": "build-s3",
        "binary": "climacarta-esp32s3.bin",
        "manifest": "manifest-esp32s3-epaper-1.54g.json",
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Package both ClimaCarta firmware variants for a GitHub release."
    )
    parser.add_argument("version", help="firmware version, with or without a leading 'v'")
    parser.add_argument(
        "--repository", default="zer0g/climacarta", help="GitHub owner/repository"
    )
    parser.add_argument("--output", type=Path, default=Path("release"))
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    args = parse_args()
    version = args.version.removeprefix("v")
    if not version:
        raise SystemExit("version cannot be empty")
    tag = f"v{version}"
    args.output.mkdir(parents=True, exist_ok=True)

    for variant in VARIANTS.values():
        source = Path(variant["build"]) / "rht_sensor.bin"
        if not source.is_file():
            raise SystemExit(f"missing firmware image: {source}")

        binary = args.output / variant["binary"]
        shutil.copyfile(source, binary)
        manifest = {
            "board": variant["board"],
            "version": version,
            "url": (
                f"https://github.com/{args.repository}/releases/download/"
                f"{tag}/{variant['binary']}"
            ),
            "sha256": sha256(binary),
        }
        manifest_path = args.output / variant["manifest"]
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"created {binary} and {manifest_path}")


if __name__ == "__main__":
    main()
