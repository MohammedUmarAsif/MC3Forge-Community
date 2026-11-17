"""Copied-profile native finish -> atomic experimental inventory -> loadout file.

Reuses storage-v2; does not mint legacy NativeResultObserver admission or write RAM.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import secrets
import uuid
from pathlib import Path

import native_inventory as inventory
from lootlab.schema_v2 import EQUIPMENT_SLOTS, Affix, Item, SchemaError
from lootlab.store_v2 import EquipmentStoreV2, canonical

POOL = (10, 25, 50, 100)
MANIFEST = "native-experimental-common-power-v1"
# Keep MANIFEST stable: it is part of already committed finish payload identities.
DYNAMO_MANIFEST = "native-common-drift-dynamo-v1"
SESSION = re.compile(r"[0-9a-f]{32}")


class NativeLoot:
    """One launcher owns this pipeline; SQLite commits precede native publication."""

    def __init__(
        self,
        runtime: Path,
        *,
        session: str | None = None,
        dynamo_drops: bool = False,
        inventory_mode: bool = False,
    ):
        self.runtime = runtime
        self.dynamo_drops = dynamo_drops
        self.inventory_mode = inventory_mode
        self.session = session or uuid.uuid4().hex
        if not SESSION.fullmatch(self.session):
            raise SchemaError("Invalid native loot session")
        path = runtime / "MC3Forge-native-loot.sqlite3"
        self.store = (
            EquipmentStoreV2(path) if path.exists() else EquipmentStoreV2.create(path)
        )
        with self.store.connection(write=True) as db:
            inventory.setup(db)
            db.execute(
                "CREATE TABLE IF NOT EXISTS native_loot_meta (profile TEXT NOT NULL)"
            )
            db.execute(
                "CREATE TABLE IF NOT EXISTS native_loot_runs (session TEXT PRIMARY KEY)"
            )
            row = db.execute("SELECT profile FROM native_loot_meta").fetchone()
            if row:
                self.profile = row[0]
            else:
                self.profile = uuid.uuid4().hex
                db.execute(
                    "INSERT INTO profiles_v2(profile_id,label) VALUES (?,?)",
                    (self.profile, "Copied native loot experiment"),
                )
                db.execute(
                    "INSERT INTO loadouts_v2 VALUES (?,?,?)",
                    (self.profile, "main", "Main"),
                )
                db.execute("INSERT INTO native_loot_meta VALUES (?)", (self.profile,))
            db.execute(
                "INSERT OR IGNORE INTO native_loot_runs VALUES (?)", (self.session,)
            )
        self.offset = 0
        self.action_offset = 0
        self.summary = {
            "delivered": 0,
            "duplicates": 0,
            "ineligible": 0,
            "invalid": 0,
            "equipment_actions": 0,
            "action_duplicates": 0,
        }
        self.export()

    def start(self) -> None:
        self.poll()  # Recover a prior persisted event before opening a new game.
        for filename, offset in (
            ("MC3Forge-race-results.jsonl", "offset"),
            ("MC3Forge-equipment-actions.jsonl", "action_offset"),
        ):
            journal = self.runtime / filename
            while journal.exists() and getattr(self, offset) < journal.stat().st_size:
                before = getattr(self, offset)
                self.poll()
                if before == getattr(self, offset):
                    raise SchemaError(
                        "Incomplete native event journal; offline recovery required"
                    )
        self._atomic("MC3Forge-loot-session.txt", self.session + "\n")

    def _dynamo_items(self, db) -> list[Item]:
        items = [
            Item.from_dict(json.loads(row[0]))
            for row in db.execute(
                "SELECT data FROM items_v2 WHERE profile_id=? ORDER BY rowid",
                (self.profile,),
            )
        ]
        return [
            item
            for item in items
            if item.kind == "nitrous"
            and item.rarity == "common"
            and item.affixes == (Affix("drift_nitro", 1),)
        ]

    def _atomic(self, name: str, text: str) -> None:
        path = self.runtime / name
        temporary = path.with_suffix(path.suffix + ".tmp")
        with temporary.open("w", encoding="ascii", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)

    def settle(self, event: dict) -> dict:
        expected = {
            "schema",
            "session",
            "sequence",
            "rank",
            "max_rank",
            "car",
            "code_mask",
        }
        if not isinstance(event, dict) or set(event) != expected:
            raise SchemaError("Unexpected native finish schema")
        if (
            type(event["schema"]) is not int
            or event["schema"] != 1
            or not isinstance(event["session"], str)
            or not SESSION.fullmatch(event["session"])
            or type(event["sequence"]) is not int
            or not 1 <= event["sequence"] <= 1000
            or type(event["rank"]) is not int
            or not 1 <= event["rank"] <= 8
            or type(event["max_rank"]) is not int
            or event["max_rank"] not in (3, 8)
            or type(event["code_mask"]) is not int
            or event["code_mask"] != 7
            or not isinstance(event["car"], str)
            or not re.fullmatch(r"vp_[a-z0-9_]{1,17}", event["car"])
        ):
            raise SchemaError("Unqualified native finish fields")
        with self.store.connection() as db:
            if not db.execute(
                "SELECT 1 FROM native_loot_runs WHERE session=?", (event["session"],)
            ).fetchone():
                raise SchemaError("Unregistered native run")
        event_id = f"{event['session']}.{event['sequence']}"

        def mutate(db):
            if event["rank"] > event["max_rank"]:
                return {"kind": "native_finish", "outcome": "ineligible"}
            count = db.execute(
                "SELECT COUNT(*) FROM items_v2 WHERE profile_id=?", (self.profile,)
            ).fetchone()[0]
            if count >= 1000:
                raise SchemaError(
                    "Prototype inventory full; event retained for recovery"
                )
            if self.inventory_mode:
                dynamo = self.dynamo_drops and not any(
                    a.key == "drift_nitro"
                    for i in inventory.items(db, self.profile)
                    for a in i.affixes
                )
                return inventory.drop(db, self.profile, event, event_id, dynamo)
            dynamo = self.dynamo_drops and not self._dynamo_items(db)
            percent = 0 if dynamo else secrets.choice(POOL)
            kind = "nitrous" if dynamo else "engine"
            item = Item(
                uuid.uuid5(uuid.NAMESPACE_URL, "mc3forge-native:" + event_id).hex,
                kind,
                "common",
                (Affix("drift_nitro", 1),)
                if dynamo
                else (Affix("power", percent / 100),),
                source_ref=f"{DYNAMO_MANIFEST if dynamo else MANIFEST}:{event_id}",
            )
            db.execute(
                "INSERT INTO items_v2(profile_id,item_id,data) VALUES (?,?,?)",
                (self.profile, item.item_id, canonical(item.to_dict())),
            )
            db.execute(
                "DELETE FROM equipment_v2 WHERE profile_id=? AND loadout_id='main' AND slot=?",
                (self.profile, kind),
            )
            db.execute(
                "INSERT INTO equipment_v2 VALUES (?,?,?,?)",
                (self.profile, "main", kind, item.item_id),
            )
            return {
                "kind": "native_finish",
                "outcome": "delivered",
                "item": item.to_dict(),
                "percent": percent,
                "component": kind,
                "event_id": event_id,
                "inventory_count": count + 1,
                "native_scope": "copied-profile-experimental; not legacy admitted loot",
            }

        receipt = self.store._transact(
            self.profile,
            "finish-" + event_id,
            {"manifest": MANIFEST, "event": event},
            mutate,
        )
        # Export CURRENT committed equipment, never an old duplicate receipt.
        self.export()
        return receipt

    def inventory_action(self, event: dict) -> dict:
        fields = {
            "schema",
            "session",
            "sequence",
            "action",
            "item",
            "slot",
            "affix",
            "revision",
            "offer",
        }
        if (
            not self.inventory_mode
            or not isinstance(event, dict)
            or set(event) != fields
            or type(event["schema"]) is not int
            or event["schema"] != 2
            or not isinstance(event["session"], str)
            or not SESSION.fullmatch(event["session"])
            or type(event["sequence"]) is not int
            or not 1 <= event["sequence"] <= 1000
            or event["action"]
            not in ("equip", "unequip", "offer", "accept", "keep", "test_kit")
            or not isinstance(event["item"], str)
            or not (event["item"] == "none" or SESSION.fullmatch(event["item"]))
            or event["slot"] not in EQUIPMENT_SLOTS
            or type(event["affix"]) is not int
            or not 0 <= event["affix"] < 8
            or type(event["revision"]) is not int
            or not 0 <= event["revision"] <= 1000000
            or not isinstance(event["offer"], str)
            or not (event["offer"] == "none" or SESSION.fullmatch(event["offer"]))
        ):
            raise SchemaError("Invalid inventory action")
        with self.store.connection() as db:
            if not db.execute(
                "SELECT 1 FROM native_loot_runs WHERE session=?", (event["session"],)
            ).fetchone():
                raise SchemaError("Unregistered inventory run")
        receipt = self.store._transact(
            self.profile,
            f"equipment-{event['session']}.{event['sequence']}",
            {"kind": "inventory_action", "event": event},
            lambda db: inventory.action(db, self.profile, event),
        )
        self.export()
        return receipt

    def equipment(self, event: dict) -> dict:
        if isinstance(event, dict) and event.get("schema") == 2:
            return self.inventory_action(event)
        if (
            not isinstance(event, dict)
            or set(event) != {"schema", "session", "sequence", "action"}
            or type(event["schema"]) is not int
            or event["schema"] != 1
            or not isinstance(event["session"], str)
            or not SESSION.fullmatch(event["session"])
            or type(event["sequence"]) is not int
            or not 1 <= event["sequence"] <= 1000
            or event["action"] not in ("equip_dynamo", "unequip_dynamo")
        ):
            raise SchemaError("Invalid native equipment action")
        with self.store.connection() as db:
            if not db.execute(
                "SELECT 1 FROM native_loot_runs WHERE session=?", (event["session"],)
            ).fetchone():
                raise SchemaError("Unregistered equipment run")

        def mutate(db):
            items = self._dynamo_items(db)
            if not items:
                raise SchemaError("No owned Dynamo item")
            item = items[0]
            db.execute(
                "DELETE FROM equipment_v2 WHERE profile_id=? AND loadout_id='main' AND slot='nitrous'",
                (self.profile,),
            )
            equipped = event["action"] == "equip_dynamo"
            if equipped:
                db.execute(
                    "INSERT INTO equipment_v2 VALUES (?,?,?,?)",
                    (self.profile, "main", "nitrous", item.item_id),
                )
            return {
                "kind": "dynamo_equipment",
                "outcome": "equipped" if equipped else "unequipped",
                "item_id": item.item_id,
            }

        receipt = self.store._transact(
            self.profile,
            f"equipment-{event['session']}.{event['sequence']}",
            {"kind": "dynamo_equipment", "event": event},
            mutate,
        )
        self.export()
        return receipt

    def export(self) -> None:
        if self.inventory_mode:
            state = self.store.state(self.profile)
            with self.store.connection() as db:
                snapshot, build = inventory.snapshot(
                    db, self.profile, state["profile"]["revision"]
                )
                equipped = [
                    Item.from_dict(data)
                    for data in state["equipped_items"].values()
                    if data
                ]
            engine_id = next(
                (
                    i.item_id
                    for i in equipped
                    if any(a.key == "power" for a in i.affixes)
                ),
                "none",
            )
            dynamo_id = next(
                (
                    i.item_id
                    for i in equipped
                    if any(a.key == "drift_nitro" for a in i.affixes)
                ),
                "none",
            )
            # UI snapshot first; CPU accepts it only at the matching loadout revision.
            self._atomic("MC3Forge-inventory.txt", snapshot)
            self._atomic(
                "MC3Forge-rolled-loadout.ini",
                "MC3FORGE_ROLLED_V3\n"
                f"revision={state['profile']['revision']}\nitem={engine_id}\npower={round(build['power'] * 100)}\nitems={state['inventory_count']}\n"
                f"dynamo_item={dynamo_id}\ndynamo={int(build['drift_nitro'] > 0)}\n"
                f"dynamo_milli={round(build['drift_nitro'] * (1 + build['drift_charge_rate']) * 1000)}\n",
            )
            return
        state = self.store.state(self.profile)
        item_data = state["equipped_items"].get("engine")
        if not state["inventory_count"]:
            return
        engine_id, percent = "none", 0
        if item_data is not None:
            item = Item.from_dict(item_data)
            if (
                item.kind != "engine"
                or item.rarity != "common"
                or len(item.affixes) != 1
            ):
                raise SchemaError("Unsupported native equipped item")
            affix = item.affixes[0]
            percent = round(affix.value * 100)
            if (
                affix.key != "power"
                or percent not in POOL
                or affix.value != percent / 100
            ):
                raise SchemaError("Unsupported native power roll")
            engine_id = item.item_id
        with self.store.connection() as db:
            items = self._dynamo_items(db)
        dynamo_id = items[0].item_id if items else "none"
        dynamo_on = state["equipped"]["nitrous"] == dynamo_id
        if state["equipped"]["nitrous"] is not None and not dynamo_on:
            raise SchemaError("Unsupported equipped nitrous item")
        packet = (
            "MC3FORGE_ROLLED_V2\n"
            f"revision={state['profile']['revision']}\n"
            f"item={engine_id}\npower={percent}\nitems={state['inventory_count']}\n"
            f"dynamo_item={dynamo_id}\ndynamo={int(dynamo_on)}\n"
        )
        self._atomic("MC3Forge-rolled-loadout.ini", packet)

    def poll(self) -> None:
        for filename, offset, operation, action in (
            ("MC3Forge-race-results.jsonl", "offset", self.settle, False),
            ("MC3Forge-equipment-actions.jsonl", "action_offset", self.equipment, True),
        ):
            path = self.runtime / filename
            if not path.exists():
                continue
            if path.stat().st_size > 16 * 1024 * 1024:
                raise SchemaError("Native event journal exceeds prototype bound")
            with path.open("rb") as stream:
                stream.seek(getattr(self, offset))
                for _ in range(64):
                    line = stream.readline(4097)
                    if not line:
                        break
                    if len(line) > 4096:
                        raise SchemaError("Oversized native event")
                    if not line.endswith(b"\n"):
                        break
                    try:
                        event = json.loads(line)
                    except (ValueError, UnicodeError) as error:
                        self.summary["invalid"] += 1
                        raise SchemaError("Malformed durable native event") from error
                    receipt = operation(event)
                    if action:
                        key = (
                            "action_duplicates"
                            if receipt["duplicate"]
                            else "equipment_actions"
                        )
                    else:
                        key = (
                            "duplicates" if receipt["duplicate"] else receipt["outcome"]
                        )
                    self.summary[key] += 1
                    setattr(self, offset, stream.tell())

    def report(self) -> dict:
        state = self.store.state(self.profile)
        return self.summary | {
            "inventory_count": state["inventory_count"],
            "equipped": state["equipped"],
            "revision": state["profile"]["revision"],
            "packet_sha256": hashlib.sha256(
                (self.runtime / "MC3Forge-rolled-loadout.ini").read_bytes()
            ).hexdigest()
            if state["inventory_count"]
            else None,
        }
