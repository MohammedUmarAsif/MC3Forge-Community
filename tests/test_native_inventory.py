"""Transactions, replay and build math for the functional inventory prototype."""

import json
from dataclasses import replace

import native_inventory as inv
import pytest
from lootlab.schema_v2 import Affix, Item, SchemaError
from lootlab.store_v2 import canonical
from native_loot import NativeLoot
from test_native_loot import event


def request(
    pipe,
    action,
    *,
    item="none",
    slot="engine",
    index=0,
    offer="none",
    revision=None,
    sequence=1,
):
    return {
        "schema": 2,
        "session": pipe.session,
        "sequence": sequence,
        "action": action,
        "item": item,
        "slot": slot,
        "affix": index,
        "offer": offer,
        "revision": pipe.report()["revision"] if revision is None else revision,
    }


def add(pipe, item, slot=None):
    def mutate(db):
        db.execute(
            "INSERT INTO items_v2(profile_id,item_id,data) VALUES (?,?,?)",
            (pipe.profile, item.item_id, canonical(item.to_dict())),
        )
        if slot:
            db.execute(
                "INSERT INTO equipment_v2 VALUES (?,?,?,?)",
                (pipe.profile, "main", slot, item.item_id),
            )
        return {"outcome": "fixture"}

    pipe.store._transact(pipe.profile, "fixture-" + item.item_id, {}, mutate)
    pipe.export()


def build(pipe):
    with pipe.store.connection() as db:
        return inv.totals(db, pipe.profile)


def offer(pipe, item):
    with pipe.store.connection() as db:
        return json.loads(
            db.execute(
                "SELECT data FROM native_offers WHERE profile=? AND item=?",
                (pipe.profile, item),
            ).fetchone()[0]
        )


def test_swaps_stack_restore_and_replay_without_compounding(tmp_path):
    pipe = NativeLoot(tmp_path, inventory_mode=True)
    engine = Item("a" * 32, "engine", "common", (Affix("power", 0.1),))
    add(pipe, engine, "engine")
    kit = request(pipe, "test_kit")
    pipe.equipment(kit)
    assert pipe.equipment(kit)["duplicate"]
    assert pipe.report()["inventory_count"] == 3
    with pipe.store.connection() as db:
        kits = [i for i in inv.items(db, pipe.profile) if i.item_id != engine.item_id]
    for seq, item in enumerate(kits, 2):
        pipe.equipment(
            request(pipe, "equip", item=item.item_id, slot=item.kind, sequence=seq)
        )
    assert build(pipe)["power"] == 0.2
    pipe.equipment(request(pipe, "unequip", slot="brakes", sequence=4))
    assert build(pipe)["power"] == 0.15
    # A stale request is a durable refusal, not a poison journal or partial swap.
    stale = request(pipe, "unequip", slot="engine", revision=0, sequence=5)
    assert pipe.equipment(stale)["outcome"] == "refused"
    assert pipe.equipment(stale)["duplicate"]
    assert build(pipe)["power"] == 0.15
    reopened = NativeLoot(tmp_path, inventory_mode=True)
    assert build(reopened)["power"] == 0.15
    pipe.equipment(kit)
    assert build(pipe)["power"] == 0.15


def test_reroll_offer_keep_accept_preserves_other_properties(tmp_path):
    pipe = NativeLoot(tmp_path, inventory_mode=True)
    item = Item(
        "a" * 32,
        "engine",
        "rare",
        (Affix("power", 0.1), Affix("luck", 0.1), Affix("loot_quantity", 0.1)),
    )
    add(pipe, item, "engine")
    pipe.equipment(request(pipe, "offer", item=item.item_id))
    candidate = offer(pipe, item.item_id)
    assert build(pipe)["power"] == 0.1
    assert candidate["key"] not in ("luck", "loot_quantity")
    pipe.equipment(
        request(pipe, "keep", item=item.item_id, offer=candidate["id"], sequence=2)
    )
    assert build(pipe)["power"] == 0.1
    pipe.equipment(request(pipe, "offer", item=item.item_id, sequence=3))
    candidate = offer(pipe, item.item_id)
    accepted = request(
        pipe, "accept", item=item.item_id, offer=candidate["id"], sequence=4
    )
    pipe.equipment(accepted)
    assert pipe.equipment(accepted)["duplicate"]
    current = pipe.store.state(pipe.profile)["equipped_items"]["engine"]
    expected = replace(
        item, affixes=(Affix(candidate["key"], candidate["value"]), *item.affixes[1:])
    )
    assert current == expected.to_dict()
    assert (
        pipe.equipment(
            request(
                pipe, "accept", item=item.item_id, offer=candidate["id"], sequence=5
            )
        )["outcome"]
        == "refused"
    )


def test_position_pity_and_quantity_use_pre_reward_build(tmp_path, monkeypatch):
    pipe = NativeLoot(tmp_path, inventory_mode=True)
    # +100% quantity across two different slots always awards two parts.
    add(
        pipe,
        Item("a" * 32, "engine", "common", (Affix("loot_quantity", 0.5),)),
        "engine",
    )
    add(
        pipe, Item("b" * 32, "tires", "common", (Affix("loot_quantity", 0.5),)), "tires"
    )
    monkeypatch.setattr(inv.secrets, "randbelow", lambda n: 0)
    monkeypatch.setattr(inv.secrets, "choice", lambda seq: seq[0])
    with pipe.store.connection(write=True) as db:
        db.execute("INSERT INTO native_progress VALUES (?,99)", (pipe.profile,))
    receipt = pipe.settle(event(pipe.session))
    assert len(receipt["drops"]) == 2
    assert receipt["drops"][0]["rarity"] == "epic" and receipt["drops"][0]["pity"]
    assert receipt["pity_dry"] == 1
    assert pipe.settle(event(pipe.session))["duplicate"]
    assert pipe.report()["inventory_count"] == 4
    # Existing loadout remains intact; no roll replaces equipped gear.
    assert pipe.report()["equipped"]["engine"] == "a" * 32
    # The same RNG sample yields Rare for first, Uncommon for last.
    monkeypatch.setattr(inv.secrets, "randbelow", lambda n: 900000)
    first = pipe.settle(event(pipe.session, 2, rank=1))
    last = pipe.settle(event(pipe.session, 3, rank=8))
    assert first["drops"][0]["rarity"] == "rare"
    assert last["drops"][0]["rarity"] == "uncommon"


def test_dynamo_copies_add_charge_rate_and_accessory_cannot_count_twice(tmp_path):
    pipe = NativeLoot(tmp_path, inventory_mode=True)
    add(
        pipe,
        Item(
            "a" * 32,
            "engine",
            "uncommon",
            (Affix("drift_nitro", 1), Affix("drift_charge_rate", 0.5)),
        ),
        "engine",
    )
    add(
        pipe,
        Item("b" * 32, "accessory", "common", (Affix("drift_nitro", 1),)),
        "accessory_1",
    )
    pipe.equipment(request(pipe, "equip", item="b" * 32, slot="accessory_2"))
    assert pipe.report()["equipped"]["accessory_1"] is None
    assert build(pipe)["drift_nitro"] == 2
    packet = (tmp_path / "MC3Forge-rolled-loadout.ini").read_text()
    assert "dynamo_milli=3000\n" in packet
    assert "item=none\npower=0" in packet


@pytest.mark.parametrize(
    "changes",
    [
        {"item": "bad"},
        {"affix": -1},
        {"revision": True},
        {"offer": "bad"},
        {"slot": "car"},
        {"action": "delete_all"},
    ],
)
def test_malformed_actions_do_not_change_items(tmp_path, changes):
    pipe = NativeLoot(tmp_path, inventory_mode=True)
    before = pipe.report()
    with pytest.raises(SchemaError):
        pipe.equipment(request(pipe, "test_kit") | changes)
    assert pipe.report() == before
