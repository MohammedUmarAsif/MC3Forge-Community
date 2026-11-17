"""Stage original addon sources into a clean, pinned upstream PCSX2 checkout."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REVISION = "fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3"
ELF_SHA256 = "ad22a30c5437e1fbb997e49ff7b3cd5bd309c37f69ff2075be32ccb05e15e45c"
GROUPS = {
    "BuildGuard": [(0x3ED9D0, 32), (0x1AE7D8, 32), (0x1AF0B0, 32)],
    "TelemetryGuard": [
        (0x2628F8, 76),
        (0x262960, 256),
        (0x262A60, 164),
        (0x262B40, 1444),
        (0x267280, 564),
        (0x433420, 112),
        (0x433490, 1224),
    ],
    "DynamoGuard": [
        (0x510508, 1688),
        (0x25F968, 60),
        (0x2DB588, 356),
        (0x2DA990, 484),
        (0x2DA610, 756),
        (0x28B068, 828),
    ],
    "MoneyGuard": [
        (0x3F4D60, 2136),
        (0x3DD8F8, 272),
        (0x4B7298, 32),
        (0x4B7188, 16),
        (0x514530, 40),
        (0x51A058, 16),
        (0x51A068, 64),
    ],
}


def elf_bytes(data: bytes, address: int, length: int) -> bytes:
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected a little-endian ELF32 input")
    offset = struct.unpack_from("<I", data, 28)[0]
    size, count = struct.unpack_from("<HH", data, 42)
    if size < 32 or count > 256 or offset + size * count > len(data):
        raise ValueError("Invalid ELF segment table")
    for index in range(count):
        kind, file_offset, virtual, _, file_size = struct.unpack_from(
            "<IIIII", data, offset + index * size
        )
        if kind == 1 and virtual <= address and address + length <= virtual + file_size:
            start = file_offset + address - virtual
            if start + length <= len(data):
                return data[start : start + length]
    raise ValueError("Required guarded range is absent")


def guard_headers(data: bytes) -> dict[str, str]:
    result = {}
    for group, spans in GROUPS.items():
        namespace = {
            "BuildGuard": "MC3ForgeBuildGuard",
            "TelemetryGuard": "MC3ForgeTelemetryGuard",
            "DynamoGuard": "MC3DynamoGuard",
            "MoneyGuard": "MC3MoneyGuard",
        }[group]
        declarations, views = [], []
        for index, (address, length) in enumerate(spans):
            raw = elf_bytes(data, address, length)
            declarations.append(
                f"inline constexpr std::array<std::uint8_t,{length}> code{index}{{"
                + ",".join(hex(b) for b in raw)
                + "};"
            )
            views.append(
                "{" + hex(address) + f",code{index}.data(),code{index}.size()" + "}"
            )
        if group == "BuildGuard":
            rows = [
                "{"
                + hex(a)
                + ",{"
                + ",".join(hex(b) for b in elf_bytes(data, a, n))
                + "}}"
                for a, n in spans
            ]
            result[group + ".h"] = (
                "#pragma once\n#include <array>\n#include <cstdint>\nnamespace MC3ForgeBuildGuard { struct Check { std::uint32_t address; std::array<std::uint8_t,32> bytes; }; inline constexpr std::array<Check,3> checks{{"
                + ",".join(rows)
                + "}}; }\n"
            )
        else:
            result[group + ".h"] = (
                "#pragma once\n#include <array>\n#include <cstdint>\n#include <cstddef>\nnamespace "
                + namespace
                + " { struct Check { std::uint32_t address; const std::uint8_t* bytes; std::size_t length; };\n"
                + "\n".join(declarations)
                + f"\ninline constexpr std::array<Check,{len(spans)}> checks"
                + "{{"
                + ",".join(views)
                + "}}; }\n"
            )
    return result


def hook_functions(text: str, signatures: list[tuple[str, str]]) -> str:
    text = '#include "MC3ForgeTrainer.h"\n' + text
    for signature, call in signatures:
        if text.count(signature) != 1:
            raise ValueError(f"Upstream hook signature changed: {signature}")
        point = text.index("{", text.index(signature)) + 1
        text = text[:point] + f"\n\tMC3ForgeTrainer::{call}();" + text[point:]
    return text


def integrate(source: Path, elf: Path, *, check_only: bool = False) -> None:
    source = source.resolve()
    revision = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != REVISION:
        raise ValueError("Upstream revision differs from the reviewed pin")
    data = elf.read_bytes()
    if hashlib.sha256(data).hexdigest() != ELF_SHA256:
        raise ValueError("Unsupported VIP ELF; no source changes made")
    headers = guard_headers(data)
    names = (
        "pcsx2/CMakeLists.txt",
        "pcsx2/VMManager.cpp",
        "pcsx2/ImGui/ImGuiManager.cpp",
        "pcsx2/SIO/Pad/PadDualshock2.cpp",
    )
    originals = {}
    for name in names:
        pristine = (
            subprocess.check_output(["git", "-C", str(source), "show", f"HEAD:{name}"])
            .decode("utf-8")
            .replace("\r\n", "\n")
        )
        path = source / name
        if (
            path.is_symlink()
            or not path.resolve().is_relative_to(source)
            or path.read_text(encoding="utf-8") != pristine
        ):
            raise ValueError(f"Preserve modified source: {name}. Use a fresh checkout.")
        originals[name] = pristine
    changes = {}
    cmake = originals[names[0]]
    if cmake.count("\tVMManager.cpp\n") != 1:
        raise ValueError("Upstream source list changed")
    changes[names[0]] = cmake.replace(
        "\tVMManager.cpp\n", "\tVMManager.cpp\n\tMC3ForgeTrainer.cpp\n", 1
    )
    changes[names[1]] = hook_functions(
        originals[names[1]],
        [
            ("void VMManager::Shutdown(", "Suspend"),
            ("void VMManager::Reset(", "Invalidate"),
            ("bool VMManager::DoLoadState(", "Invalidate"),
            ("void VMManager::DoSaveState(", "Suspend"),
            ("void VMManager::Internal::VSyncOnCPUThread(", "Tick"),
        ],
    )
    changes[names[2]] = hook_functions(
        originals[names[2]], [("void ImGuiManager::RenderOSD(", "Render")]
    )
    poll = (
        "u8 PadDualshock2::Poll(u8 commandByte)\n{\n\tconst u32 buttons = GetButtons();"
    )
    text = originals[names[3]]
    if text.count(poll) != 1:
        raise ValueError("Upstream pad poll changed")
    text = '#include "MC3ForgeTrainer.h"\n' + text.replace(
        poll,
        "u8 PadDualshock2::Poll(u8 commandByte)\n{\n\tu32 held = 0;\n\tfor (u32 i = 0; i < Inputs::LENGTH; ++i) if (rawInputs[i] > 32) held |= (1u << i);\n\tconst bool equipment_capture = unifiedSlot == 0 && MC3ForgeTrainer::CaptureEquipmentInput(held);\n\tconst u32 buttons = equipment_capture ? 0xffffffffu : GetButtons();",
        1,
    )
    for axis in ("PAD_R_RIGHT", "PAD_R_UP", "PAD_L_RIGHT", "PAD_L_UP"):
        before = f"return GetPressure(Inputs::{axis});"
        if text.count(before) != 1:
            raise ValueError("Upstream analog packet changed")
        text = text.replace(
            before, f"return equipment_capture ? 127 : GetPressure(Inputs::{axis});", 1
        )
    changes[names[3]] = text
    native_files = list((ROOT / "native").glob("*.h")) + list(
        (ROOT / "native").glob("*.cpp")
    )
    for path in native_files:
        target = source / "pcsx2" / path.name
        if target.exists():
            raise ValueError(f"Preserve existing addon file: {path.name}")
    for name in headers:
        if (source / "pcsx2" / name).exists():
            raise ValueError("Preserve existing generated guards; use a fresh checkout")
    if check_only:
        print("Pinned source/ELF/hook checks passed; no changes written")
        return
    for name, text in changes.items():
        (source / name).write_text(text, encoding="utf-8", newline="\n")
    for path in native_files:
        shutil.copy2(path, source / "pcsx2" / path.name)
    for name, text in headers.items():
        (source / "pcsx2" / name).write_text(text, encoding="utf-8")
    print(
        "Original addon sources staged. Generated guard bytes stay local; do not publish them."
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pcsx2-source", type=Path, required=True)
    parser.add_argument("--vip-elf", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    integrate(args.pcsx2_source, args.vip_elf, check_only=args.check_only)
