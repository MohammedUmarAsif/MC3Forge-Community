"""Isolated Windows setup/launcher for a user-built compatible emulator."""

from __future__ import annotations

import argparse
import configparser
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from profile_lock import lock_profile

ROOT = Path(__file__).resolve().parents[1]
PLAY = ROOT / ".local/play"
sys.path.insert(0, str(ROOT / "runtime"))
from native_loot import NativeLoot  # noqa: E402


def safe_file(path: Path, parent: Path) -> None:
    if path.is_symlink() or not path.resolve().is_relative_to(parent.resolve()):
        raise ValueError("Symlink or external runtime input refused")


def has_addon(exe: Path) -> bool:
    if not exe.is_file() or exe.stat().st_size > 256 * 1024 * 1024:
        return False
    with exe.open("rb") as stream:
        tail = b""
        while block := stream.read(1024 * 1024):
            combined = tail + block
            if b"MC3FORGE_CPU_TRAINER_V1" in combined:
                return True
            tail = combined[-64:]
    return False


def setup(emulator: Path, iso: Path, bios: Path, card: Path) -> None:
    if not PLAY.resolve().is_relative_to(ROOT.resolve()) or PLAY.exists():
        raise ValueError(
            "Destination exists or is redirected. Preserve it; setup never overwrites a profile."
        )
    if not has_addon(emulator / "pcsx2-qt.exe"):
        raise ValueError(
            "A compatible custom build is required; stock PCSX2 is unsupported"
        )
    if not all(path.is_file() and not path.is_symlink() for path in (iso, bios, card)):
        raise ValueError("Select existing regular ISO, BIOS and card files")
    if iso.suffix.lower() != ".iso" or card.suffix.lower() != ".ps2":
        raise ValueError("This preview requires an ISO and a .ps2 memory card")
    if (
        not 1024 * 1024 <= bios.stat().st_size <= 8 * 1024 * 1024
        or not 8 * 1024 * 1024 <= card.stat().st_size <= 128 * 1024 * 1024
    ):
        raise ValueError("Unexpected BIOS/card size")
    files = [
        p
        for p in emulator.iterdir()
        if p.is_file()
        and (p.suffix.lower() == ".dll" or p.name in ("pcsx2-qt.exe", "qt.conf"))
    ]
    folders = [
        emulator / name
        for name in ("resources", "QtPlugins", "translations", "D3D12")
        if (emulator / name).is_dir()
    ]
    for path in files + folders:
        safe_file(path, emulator)
    for folder in folders:
        for path in folder.rglob("*"):
            safe_file(path, emulator)
    PLAY.mkdir(parents=True)
    for path in files:
        shutil.copy2(path, PLAY / path.name)
    for folder in folders:
        shutil.copytree(folder, PLAY / folder.name)
    for name in (
        "inis",
        "bios",
        "memcards",
        "temp",
        "home",
        "logs",
        "sstates",
        "snaps",
        "cache",
    ):
        (PLAY / name).mkdir(exist_ok=True)
    shutil.copy2(bios, PLAY / "bios" / bios.name)
    shutil.copy2(card, PLAY / "memcards/MC3Forge-copy.ps2")
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = lambda optionstr: optionstr
    config.read_dict(
        {
            "UI": {"ConfirmShutdown": "false", "CheckForUpdatesAtStartup": "false"},
            "Folders": {
                "Bios": "bios",
                "MemoryCards": "memcards",
                "Logs": "logs",
                "SaveStates": "sstates",
                "Snapshots": "snaps",
                "Cache": "cache",
            },
            "Filenames": {"BIOS": bios.name},
            "MemoryCards": {
                "Slot1_Enable": "true",
                "Slot1_Filename": "MC3Forge-copy.ps2",
                "Slot2_Enable": "false",
            },
            "EmuCore": {"EnablePINE": "false"},
            "EmuCore/CPU/Recompiler": {"EnableEECache": "false"},
            "EmuCore/GS": {"upscale_multiplier": "1"},
        }
    )
    with (PLAY / "inis/PCSX2.ini").open("w", encoding="utf-8") as output:
        config.write(output)
    (PLAY / "portable.ini").touch()
    (PLAY / "MC3Forge-trainer.enable").write_text("MC3FORGE_CPU_TRAINER_V1\n")
    (PLAY / "launch.json").write_text(json.dumps({"iso": str(iso.resolve())}))
    print("Created .local/play with copied BIOS/card. Originals were not modified.")


def run(*, experimental: bool) -> None:
    if not experimental:
        raise ValueError(
            "This preview writes gameplay and wallet state in its copied profile. Add --experimental to run."
        )
    if not (PLAY / "launch.json").is_file() or not has_addon(PLAY / "pcsx2-qt.exe"):
        raise ValueError("Run setup first")
    if not PLAY.resolve().is_relative_to(ROOT.resolve()):
        raise ValueError("Redirected profile refused")
    with (PLAY / "MC3Forge.lock").open("a+b") as lock:
        lock_profile(lock)
        config = configparser.ConfigParser(interpolation=None)
        config.read(PLAY / "inis/PCSX2.ini", encoding="utf-8")
        if (
            config.getboolean("EmuCore", "EnablePINE")
            or config.get("MemoryCards", "Slot1_Filename") != "MC3Forge-copy.ps2"
            or config.getboolean("MemoryCards", "Slot2_Enable")
        ):
            raise ValueError("Unsafe changed endpoint/card configuration refused")
        for _, folder in config.items("Folders"):
            if not (PLAY / folder).resolve().is_relative_to(PLAY.resolve()):
                raise ValueError("External profile folder refused")
        pipe = NativeLoot(PLAY, inventory_mode=True)
        pipe.start()
        iso = Path(json.loads((PLAY / "launch.json").read_text())["iso"])
        if not iso.is_file():
            raise ValueError("Selected ISO is no longer available")
        windows = os.environ["SystemRoot"]
        env = {
            "SystemRoot": windows,
            "WINDIR": windows,
            "PATH": str(PLAY) + os.pathsep + str(Path(windows) / "System32"),
            "TEMP": str(PLAY / "temp"),
            "TMP": str(PLAY / "temp"),
            "USERPROFILE": str(PLAY / "home"),
            "APPDATA": str(PLAY / "home"),
            "LOCALAPPDATA": str(PLAY / "home"),
        }
        with (PLAY / "launcher.log").open("w") as log:
            child = subprocess.Popen(
                [
                    str(PLAY / "pcsx2-qt.exe"),
                    "-portable",
                    "-fastboot",
                    "-nofullscreen",
                    "--",
                    str(iso),
                ],
                cwd=PLAY,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                while child.poll() is None:
                    pipe.poll()
                    time.sleep(0.1)
            finally:
                if child.poll() is None:
                    print(
                        "Companion interrupted; close the isolated emulator normally before another launch."
                    )
                    child.wait()
            pipe.poll()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("setup")
    for name in ("emulator", "iso", "bios", "card"):
        create.add_argument("--" + name, type=Path, required=True)
    launch = commands.add_parser("run")
    launch.add_argument("--experimental", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "setup":
            setup(args.emulator, args.iso, args.bios, args.card)
        else:
            run(experimental=args.experimental)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Refused: {error}\n")
