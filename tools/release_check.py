"""Fail closed on unexpected tracked payloads and common accidental secret leaks."""

from __future__ import annotations

import hashlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROOT_FILES = {
    ".gitignore",
    ".gitattributes",
    "pyproject.toml",
    "uv.lock",
    "README.md",
    "LICENSE",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "CHANGELOG.md",
}
DIRECTORIES = {"runtime", "native", "tools", "tests", "docs", ".github"}
EXTENSIONS = {".py", ".h", ".cpp", ".md", ".yml", ".yaml", ".png"}
PATTERNS = [
    re.compile(rb"\bgh[pousr]_[A-Za-z0-9_]{20,}"),
    re.compile(rb"\bgithub_pat_[A-Za-z0-9_]{20,}"),
    re.compile(rb"\bsk-[A-Za-z0-9_-]{20,}"),
    re.compile(rb"-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----"),
    re.compile(
        rb"(?:C:[\\/]Users[\\/][^\\/\s]+|D:[\\/]Projects[\\/]Active[\\/])", re.I
    ),
]


def inspect(relative: str, data: bytes) -> None:
    path = Path(relative)
    if len(path.parts) == 1:
        if relative not in ROOT_FILES:
            raise ValueError(f"Unexpected root file: {relative}")
    elif path.parts[0] not in DIRECTORIES or path.suffix not in EXTENSIONS:
        raise ValueError(f"Unexpected payload: {relative}")
    if path.name.endswith("Guard.h"):
        raise ValueError("Generated guard data must stay local")
    if len(data) > 2 * 1024 * 1024:
        raise ValueError(f"Oversized source/artifact: {relative}")
    if path.suffix == ".png":
        if path.parts[:2] != ("docs", "images") or not data.startswith(
            b"\x89PNG\r\n\x1a\n"
        ):
            raise ValueError("Only reviewed docs/images PNG previews are allowed")
    else:
        text = data.decode("utf-8")
        if any(pattern.search(data) for pattern in PATTERNS):
            raise ValueError(
                f"Possible secret or private host path in {relative}; inspect locally"
            )
        if path.suffix == ".md":
            for target in re.findall(r"\]\(([^)]+)\)", text):
                if target.startswith(("https://", "http://", "#")):
                    continue
                linked = (ROOT / path.parent / target.split("#", 1)[0]).resolve()
                if not linked.is_relative_to(ROOT) or not linked.exists():
                    raise ValueError(
                        f"Broken/internal external-path link in {relative}"
                    )


def main() -> None:
    names = (
        subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
        .decode()
        .split("\0")
    )
    manifest = []
    for relative in filter(None, names):
        path = ROOT / relative
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT):
            raise ValueError("Tracked symlink/external payload refused")
        data = path.read_bytes()
        inspect(relative, data)
        manifest.append((relative, len(data), hashlib.sha256(data).hexdigest()))
    if not manifest:
        raise ValueError("No tracked release files")
    print(
        f"Reviewed tracked allowlist: {len(manifest)} files, {sum(row[1] for row in manifest)} bytes; no blocked payload/signature found."
    )
    print("This check is not a complete secret, licensing or gameplay certification.")


if __name__ == "__main__":
    main()
