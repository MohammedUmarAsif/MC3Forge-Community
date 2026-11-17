"""Prototype item pools and transactions; no game cash, RAM or set writes."""

from __future__ import annotations

import hashlib
import json
import math
import secrets
import uuid
from dataclasses import replace

from lootlab.schema_v2 import COMPONENTS, Affix, Item, SchemaError
from lootlab.store_v2 import canonical

# Fractions internally; plain percentages in the native power packet.
POOLS = {
    "power": (0.02, 0.05, 0.10),
    "drift_nitro": (1.0,),
    "drift_charge_rate": (0.10, 0.25, 0.50),
    "luck": (0.05, 0.10, 0.20),
    "loot_quantity": (0.05, 0.10, 0.20),
    "pity_rate": (0.05, 0.10, 0.20),
    "money": (0.05, 0.10, 0.25),
}
RARITIES = ("common", "uncommon", "rare", "epic")
PITY = 100  # Test policy, not a balance claim; awarded parts, not race count.


def pity_threshold(build):
    """Equipped acceleration reduces the awarded-part threshold; no retroactive rolls."""
    return max(1, math.ceil(PITY / (1 + build.get("pity_rate", 0))))


def setup(db):
    db.execute(
        "CREATE TABLE IF NOT EXISTS native_progress(profile TEXT PRIMARY KEY, dry INTEGER NOT NULL DEFAULT 0)"
    )
    db.execute(
        "CREATE TABLE IF NOT EXISTS native_offers(profile TEXT, item TEXT, data TEXT, PRIMARY KEY(profile,item))"
    )


def items(db, profile):
    return [
        Item.from_dict(json.loads(r[0]))
        for r in db.execute(
            "SELECT data FROM items_v2 WHERE profile_id=? ORDER BY rowid", (profile,)
        )
    ]


def totals(db, profile):
    equipped = {
        r[0]
        for r in db.execute(
            "SELECT item_id FROM equipment_v2 WHERE profile_id=? AND loadout_id='main'",
            (profile,),
        )
    }
    result = dict.fromkeys(POOLS, 0.0)
    for item in items(db, profile):
        if item.item_id not in equipped:
            continue
        for affix in item.affixes:
            if affix.key not in POOLS or not 0 <= affix.value <= (
                1 if affix.key in ("power", "drift_nitro", "money") else 0.5
            ):
                raise SchemaError("Unsupported native equipped affix")
            result[affix.key] += affix.value
    # Fixed-point native boundary avoids floating roundoff and display disagreement.
    result = {k: round(v, 6) for k, v in result.items()}
    if result["power"] > 8:
        raise SchemaError("Build exceeds prototype power validation bound")
    return result


def drop(db, profile, event, event_id, guaranteed_dynamo):
    build = totals(db, profile)  # Newly won affixes affect the next race's drops.
    count = db.execute(
        "SELECT COUNT(*) FROM items_v2 WHERE profile_id=?", (profile,)
    ).fetchone()[0]
    db.execute("INSERT OR IGNORE INTO native_progress(profile) VALUES (?)", (profile,))
    dry = db.execute(
        "SELECT dry FROM native_progress WHERE profile=?", (profile,)
    ).fetchone()[0]
    quantity = build["loot_quantity"]
    number = (
        1 + int(quantity) + int(secrets.randbelow(1000000) < (quantity % 1) * 1000000)
    )
    if count + number > 1000:
        raise SchemaError("Prototype inventory full; event retained for recovery")
    drops = []
    for index in range(number):
        weights = (
            (60, 25, 12, 3)
            if event["rank"] == 1
            else (70, 20, 8, 2)
            if event["rank"] <= 3
            else (80, 15, 4, 1)
        )
        weights = [
            weights[0],
            weights[1],
            weights[2] * (1 + build["luck"]),
            weights[3] * (1 + build["luck"]),
        ]
        draw = secrets.randbelow(1000000) / 1000000 * sum(weights)
        tier = 0
        while tier < 3 and draw >= weights[tier]:
            draw -= weights[tier]
            tier += 1
        pity = dry + 1 >= pity_threshold(build)
        if pity:
            tier = 3
        dry = 0 if tier == 3 else dry + 1
        keys = list(POOLS)
        selected = []
        if guaranteed_dynamo and index == 0:
            selected.append("drift_nitro")
            keys.remove("drift_nitro")
        while len(selected) < (6 if tier == 3 else tier + 1):
            key = secrets.choice(keys)
            selected.append(key)
            keys.remove(key)
        item = Item(
            uuid.uuid5(
                uuid.NAMESPACE_URL, f"mc3forge-inventory:{event_id}:{index}"
            ).hex,
            "nitrous"
            if guaranteed_dynamo and index == 0
            else secrets.choice((*COMPONENTS, "accessory")),
            RARITIES[tier],
            tuple(Affix(key, secrets.choice(POOLS[key])) for key in selected),
            source_ref=f"native-inventory-prototype-v1:{event_id}:{index}",
        )
        db.execute(
            "INSERT INTO items_v2(profile_id,item_id,data) VALUES (?,?,?)",
            (profile, item.item_id, canonical(item.to_dict())),
        )
        slot = item.kind if item.kind != "accessory" else "accessory_1"
        if not db.execute(
            "SELECT 1 FROM equipment_v2 WHERE profile_id=? AND loadout_id='main' AND slot=?",
            (profile, slot),
        ).fetchone():
            db.execute(
                "INSERT INTO equipment_v2 VALUES (?,?,?,?)",
                (profile, "main", slot, item.item_id),
            )
        drops.append(item.to_dict() | {"pity": pity})
    db.execute("UPDATE native_progress SET dry=? WHERE profile=?", (dry, profile))
    return {
        "kind": "native_finish",
        "outcome": "delivered",
        "item": {k: v for k, v in drops[0].items() if k != "pity"},
        "drops": drops,
        "inventory_count": count + number,
        "component": drops[0]["kind"],
        "pity_dry": dry,
        "policy": "prototype; free rerolls; no native cash debit",
    }


def action(db, profile, event):
    def refuse(reason):
        return {"kind": "inventory_action", "outcome": "refused", "reason": reason}

    revision = db.execute(
        "SELECT revision FROM profiles_v2 WHERE profile_id=?", (profile,)
    ).fetchone()[0]
    if event["revision"] != revision:
        return refuse("Inventory changed; refresh selection and retry")
    owned = {i.item_id: i for i in items(db, profile)}
    item = owned.get(event["item"])
    operation = event["action"]
    if operation == "test_kit":
        if len(owned) > 998:
            return refuse("No room for validation kit")
        for kind in ("tires", "brakes"):
            identifier = uuid.uuid5(
                uuid.NAMESPACE_URL, f"mc3forge-validation-kit:{profile}:{kind}"
            ).hex
            if identifier not in owned:
                kit = Item(
                    identifier,
                    kind,
                    "common",
                    (Affix("power", 0.05),),
                    source_ref="validation-kit-not-race-earned",
                )
                db.execute(
                    "INSERT INTO items_v2(profile_id,item_id,data) VALUES (?,?,?)",
                    (profile, identifier, canonical(kit.to_dict())),
                )
        return {"kind": "inventory_action", "outcome": "test_kit_created"}
    if operation == "unequip":
        db.execute(
            "DELETE FROM equipment_v2 WHERE profile_id=? AND loadout_id='main' AND slot=?",
            (profile, event["slot"]),
        )
    elif item is None:
        return refuse("Part is not owned")
    elif operation == "equip":
        if not item.fits(event["slot"]):
            return refuse("Part does not fit this slot")
        db.execute(
            "DELETE FROM equipment_v2 WHERE profile_id=? AND loadout_id='main' AND (slot=? OR item_id=?)",
            (profile, event["slot"], item.item_id),
        )
        db.execute(
            "INSERT INTO equipment_v2 VALUES (?,?,?,?)",
            (profile, "main", event["slot"], item.item_id),
        )
    elif operation == "offer":
        index = event["affix"]
        if index >= len(item.affixes):
            return refuse("Affix no longer exists")
        occupied = {a.key for n, a in enumerate(item.affixes) if n != index}
        candidates = [
            Affix(k, v)
            for k, values in POOLS.items()
            for v in values
            if k not in occupied and Affix(k, v) != item.affixes[index]
        ]
        if not candidates:
            return refuse(
                "Fixed Dynamo enable has no alternate roll on a full six-affix part"
            )
        candidate = secrets.choice(candidates)
        offer = {
            "id": uuid.uuid4().hex,
            "index": index,
            "before": hashlib.sha256(canonical(item.to_dict()).encode()).hexdigest(),
            "key": candidate.key,
            "value": candidate.value,
        }
        db.execute(
            "INSERT OR REPLACE INTO native_offers VALUES (?,?,?)",
            (profile, item.item_id, canonical(offer)),
        )
    elif operation in ("accept", "keep"):
        row = db.execute(
            "SELECT data FROM native_offers WHERE profile=? AND item=?",
            (profile, item.item_id),
        ).fetchone()
        if not row:
            return refuse("No pending offer")
        offer = json.loads(row[0])
        if (
            event["offer"] != offer["id"]
            or offer["before"]
            != hashlib.sha256(canonical(item.to_dict()).encode()).hexdigest()
        ):
            return refuse("Offer is stale")
        if operation == "accept":
            affixes = list(item.affixes)
            affixes[offer["index"]] = Affix(offer["key"], offer["value"])
            updated = replace(item, affixes=tuple(affixes))
            db.execute(
                "UPDATE items_v2 SET data=? WHERE profile_id=? AND item_id=?",
                (canonical(updated.to_dict()), profile, item.item_id),
            )
        db.execute(
            "DELETE FROM native_offers WHERE profile=? AND item=?",
            (profile, item.item_id),
        )
    return {"kind": "inventory_action", "outcome": operation, "item_id": event["item"]}


def snapshot(db, profile, revision):
    build = totals(db, profile)
    slots = {
        r[0]: r[1]
        for r in db.execute(
            "SELECT item_id,slot FROM equipment_v2 WHERE profile_id=? AND loadout_id='main'",
            (profile,),
        )
    }
    dry = db.execute(
        "SELECT dry FROM native_progress WHERE profile=?", (profile,)
    ).fetchone()
    last = db.execute(
        "SELECT receipt FROM journal_v2 WHERE profile_id=? ORDER BY rowid DESC LIMIT 1",
        (profile,),
    ).fetchone()
    receipt = json.loads(last[0]) if last else {}
    feedback = receipt.get("reason", receipt.get("outcome", "Ready"))
    lines = [
        "MC3FORGE_INVENTORY_V1",
        str(revision),
        str(dry[0] if dry else 0),
        feedback,
    ]
    for item in items(db, profile):
        affixes = ",".join(f"{a.key}={a.value:g}" for a in item.affixes)
        row = db.execute(
            "SELECT data FROM native_offers WHERE profile=? AND item=?",
            (profile, item.item_id),
        ).fetchone()
        offer = json.loads(row[0]) if row else None
        offer_text = (
            f"{offer['id']},{offer['index']},{offer['key']},{offer['value']:g}"
            if offer
            else "none"
        )
        label = (
            "dev-kit"
            if item.source_ref == "dev-overdrive-pack-v1"
            else "test-kit"
            if item.source_ref == "validation-kit-not-race-earned"
            else "earned"
        )
        lines.append(
            "|".join(
                (
                    item.item_id,
                    item.kind,
                    item.rarity,
                    slots.get(item.item_id, "none"),
                    affixes,
                    offer_text,
                    label,
                )
            )
        )
    return "\n".join(lines) + "\n", build
