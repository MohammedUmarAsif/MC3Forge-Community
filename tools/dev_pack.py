"""Opt-in temporary DEV pieces; never auto-equips or changes a game's wallet."""

from __future__ import annotations

import argparse
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime"))
from lootlab.schema_v2 import EQUIPMENT_SLOTS, Affix, Item  # noqa: E402
from lootlab.store_v2 import canonical  # noqa: E402
from native_loot import NativeLoot  # noqa: E402


def grant(pipe: NativeLoot) -> dict:
    def mutate(db):
        count = db.execute(
            "SELECT count(*) FROM items_v2 WHERE profile_id=?", (pipe.profile,)
        ).fetchone()[0]
        if count > 992:
            raise ValueError("No room for eight DEV pieces")
        for slot in EQUIPMENT_SLOTS:
            item = Item(
                uuid.uuid5(
                    uuid.NAMESPACE_URL, f"public-dev-overdrive-v1:{pipe.profile}:{slot}"
                ).hex,
                "accessory" if slot.startswith("accessory") else slot,
                "epic",
                tuple(
                    Affix(key, value)
                    for key, value in (
                        ("power", 1),
                        ("drift_nitro", 1),
                        ("drift_charge_rate", 0.5),
                        ("luck", 0.2),
                        ("loot_quantity", 0.2),
                        ("money", 0.25),
                    )
                ),
                source_ref="dev-overdrive-pack-v1",
            )
            db.execute(
                "INSERT INTO items_v2(profile_id,item_id,data) VALUES (?,?,?)",
                (pipe.profile, item.item_id, canonical(item.to_dict())),
            )
        return {"outcome": "eight_unequipped_dev_parts_added", "added": 8}

    receipt = pipe.store._transact(
        pipe.profile,
        "public-dev-overdrive-v1",
        {"kind": "public-dev-overdrive-v1"},
        mutate,
    )
    pipe.export()
    return receipt


if __name__ == "__main__":
    import msvcrt

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, default=ROOT / ".local/play")
    args = parser.parse_args()
    if not (args.runtime / "portable.ini").is_file():
        parser.error("Run player.py setup first")
    with (args.runtime / "MC3Forge.lock").open("a+b") as lock:
        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        result = grant(NativeLoot(args.runtime, inventory_mode=True))
        print(
            "DEV pack already exists; no duplicates added."
            if result["duplicate"]
            else "Eight DEV pieces added, unequipped. They are intentionally unbalanced."
        )
