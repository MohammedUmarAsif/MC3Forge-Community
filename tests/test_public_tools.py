import struct
from pathlib import Path

import player
import pytest
from dev_pack import grant
from integrate_pcsx2 import elf_bytes, hook_functions
from native_loot import NativeLoot


def test_dev_pack_is_opt_in_unequipped_and_idempotent(tmp_path):
    pipe = NativeLoot(tmp_path, inventory_mode=True)
    assert grant(pipe)["added"] == 8
    before = pipe.report()
    assert before["inventory_count"] == 8 and all(
        item is None for item in before["equipped"].values()
    )
    assert grant(pipe)["duplicate"] and pipe.report() == before


def test_elf_reader_bounds_and_hook_uniqueness():
    data = bytearray(128)
    data[:6] = b"\x7fELF\x01\x01"
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<HH", data, 42, 32, 1)
    struct.pack_into("<IIIII", data, 52, 1, 100, 0x100000, 0x100000, 16)
    data[100:116] = bytes(range(16))
    assert elf_bytes(bytes(data), 0x100004, 4) == bytes(range(4, 8))
    with pytest.raises(ValueError):
        elf_bytes(bytes(data), 0x10000F, 8)
    hooked = hook_functions("void f() {}", [("void f(", "Tick")])
    assert hooked.count("MC3ForgeTrainer::Tick();") == 1
    assert "void f()" in hooked
    with pytest.raises(ValueError):
        hook_functions("void f() {} void f() {}", [("void f(", "Tick")])


def test_setup_rejects_stock_emulator_without_creating_profile(tmp_path, monkeypatch):
    monkeypatch.setattr(player, "ROOT", tmp_path)
    monkeypatch.setattr(player, "PLAY", tmp_path / ".local/play")
    with pytest.raises(ValueError, match="custom build"):
        player.setup(
            tmp_path, tmp_path / "x.iso", tmp_path / "bios.bin", tmp_path / "card.ps2"
        )
    assert not player.PLAY.exists()


def test_setup_refuses_existing_profile(tmp_path, monkeypatch):
    monkeypatch.setattr(player, "ROOT", tmp_path)
    monkeypatch.setattr(player, "PLAY", tmp_path / "play")
    player.PLAY.mkdir()
    saved = player.PLAY / "keep"
    saved.write_text("existing")
    with pytest.raises(ValueError, match="Destination exists"):
        player.setup(
            tmp_path, tmp_path / "x.iso", tmp_path / "bios.bin", tmp_path / "card.ps2"
        )
    assert saved.read_text() == "existing"


def test_profile_lock_rejects_non_windows(tmp_path, monkeypatch):
    import profile_lock

    monkeypatch.setattr(profile_lock.sys, "platform", "linux")
    with (tmp_path / "lock").open("a+b") as lock:
        with pytest.raises(ValueError, match="Windows only"):
            profile_lock.lock_profile(lock)


def test_setup_copies_inputs_without_modifying_originals(tmp_path, monkeypatch):
    import hashlib
    import json

    monkeypatch.setattr(player, "ROOT", tmp_path)
    monkeypatch.setattr(player, "PLAY", tmp_path / ".local/play")
    emulator = tmp_path / "custom-runtime"
    emulator.mkdir()
    (emulator / "pcsx2-qt.exe").write_bytes(b"synthetic MC3FORGE_CPU_TRAINER_V1")
    iso, bios, card = (tmp_path / name for name in ("own.iso", "own.bin", "own.ps2"))
    iso.write_bytes(b"synthetic fixture only")
    bios.write_bytes(bytes(1024 * 1024))
    card.write_bytes(bytes(8 * 1024 * 1024))
    before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (iso, bios, card)}
    player.setup(emulator, iso, bios, card)
    assert all(
        hashlib.sha256(p.read_bytes()).hexdigest() == value
        for p, value in before.items()
    )
    assert (
        player.PLAY / "memcards/MC3Forge-copy.ps2"
    ).read_bytes() == card.read_bytes()
    assert (
        Path(json.loads((player.PLAY / "launch.json").read_text())["iso"])
        == iso.resolve()
    )
