#!/usr/bin/env python3
"""
Build script for the XeroxClient Bedrock resource pack.

Packs the resource-pack files that live at the repository root into a single
`.mcpack` file inside the `dist/` folder.

Usage:
    python3 tools/package.py                # build dist/XeroxClient-v<version>.mcpack
    python3 tools/package.py --zip          # build a .zip instead of .mcpack
    python3 tools/package.py --out out.mcpack
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Files / folders that belong to the repository, not to the resource pack.
EXCLUDED_TOP_LEVEL = {
    ".git",
    ".github",
    ".gitignore",
    ".gitattributes",
    "dist",
    "docs",
    "tools",
    "README.md",
    "NOTICE.md",
    "CHANGELOG.md",
}


def strip_json_comments(text: str) -> str:
    """Mojang manifests are JSON but some packs ship `//` comments in them."""
    # Remove block comments, then line comments, ignoring // inside strings.
    out: list[str] = []
    in_string = False
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if in_string:
            out.append(ch)
            if ch == "\\":
                if nxt:
                    out.append(nxt)
                    i += 2
                    continue
            elif ch == '"':
                in_string = False
        else:
            if ch == '"':
                in_string = True
                out.append(ch)
            elif ch == "/" and nxt == "/":
                while i < len(text) and text[i] not in "\r\n":
                    i += 1
                continue
            elif ch == "/" and nxt == "*":
                i += 2
                while i + 1 < len(text) and not (text[i] == "*" and text[i + 1] == "/"):
                    i += 1
                i += 2
                continue
            else:
                out.append(ch)
        i += 1
    return "".join(out)


def read_manifest() -> dict:
    raw = (REPO_ROOT / "manifest.json").read_text(encoding="utf-8-sig")
    cleaned = strip_json_comments(raw)
    cleaned = re.sub(r",(\s*[}\]])", r"\1", cleaned)  # trailing commas
    return json.loads(cleaned)


def pack_files() -> list[Path]:
    files: list[Path] = []
    for path in sorted(REPO_ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(REPO_ROOT)
        if rel.parts and rel.parts[0] in EXCLUDED_TOP_LEVEL:
            continue
        if path.name.endswith((".pyc", ".DS_Store")):
            continue
        files.append(path)
    return files


def version_string(manifest: dict) -> str:
    version = manifest.get("header", {}).get("version", [1, 0, 0])
    return ".".join(str(part) for part in version)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package the XeroxClient resource pack.")
    parser.add_argument("--out", help="output file path", default=None)
    parser.add_argument("--zip", action="store_true", help="produce a .zip instead of a .mcpack")
    args = parser.parse_args()

    manifest = read_manifest()
    version = version_string(manifest)

    suffix = ".zip" if args.zip else ".mcpack"
    out_path = Path(args.out) if args.out else REPO_ROOT / "dist" / f"XeroxClient-v{version}{suffix}"
    if not out_path.is_absolute():
        out_path = REPO_ROOT / out_path
    out_path.parent.mkdir(parents=True, exist_ok=True)

    files = pack_files()
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            archive.write(path, path.relative_to(REPO_ROOT).as_posix())

    size_mb = out_path.stat().st_size / (1024 * 1024)
    print(f"Packed {len(files)} files -> {out_path} ({size_mb:.2f} MiB)")
    print(f"Pack: {manifest['header'].get('name', 'unknown')} v{version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
