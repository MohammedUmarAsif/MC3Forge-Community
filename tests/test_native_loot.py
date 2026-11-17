import json

import native_loot as loot
import pytest
from lootlab.schema_v2 import SchemaError


def event(session, sequence=1, **changes):
    return {
        "schema": 1,
        "session": session,
        "sequence": sequence,
        "rank": 2,
        "max_rank": 8,
        "car": "vp_d_240sx_98",
        "code_mask": 7,
    } | changes


def equipment_event(session, sequence=1, *, equip=False):
    return {
        "schema": 1,
        "session": session,
        "sequence": sequence,
        "action": "equip_dynamo" if equip else "unequip_dynamo",
    }


def test_dynamo_drop_preserves_engine_and_equipment_survives_replay_restart(tmp_path):
    legacy = loot.NativeLoot(tmp_path)
    engine = legacy.settle(event(legacy.session))
    pipe = loot.NativeLoot(tmp_path, dynamo_drops=True)
    assert pipe.settle(event(legacy.session))[
        "duplicate"
    ]  # Old payload namespace is unchanged.
    nitro = pipe.settle(event(pipe.session))
    assert nitro["component"] == "nitrous" and nitro["item"]["affixes"] == [
        {"key": "drift_nitro", "value": 1.0}
    ]
    assert pipe.settle(event(pipe.session))["duplicate"]
    state = pipe.store.state(pipe.profile)
    assert (
        state["inventory_count"] == 2
        and state["equipped"]["engine"] == engine["item"]["item_id"]
    )
    assert state["equipped"]["nitrous"] == nitro["item"]["item_id"]
    off = equipment_event(pipe.session)
    pipe.equipment(off)
    off_packet = (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes()
    assert b"dynamo=0\n" in off_packet
    assert pipe.equipment(off)["duplicate"]
    reopened = loot.NativeLoot(tmp_path, dynamo_drops=True)
    assert reopened.store.state(reopened.profile)["equipped"]["nitrous"] is None
    assert (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes() == off_packet
    reopened.equipment(equipment_event(reopened.session, equip=True))
    on_packet = (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes()
    reopened.equipment(off)  # Old unequip replay cannot undo the newer equip.
    assert (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes() == on_packet
    assert b"dynamo=1\n" in on_packet
    later = reopened.settle(event(reopened.session, 2))
    assert (
        later["component"] == "engine"
    )  # Guaranteed Dynamo does not duplicate every race.
    assert reopened.report()["equipped"]["nitrous"] == nitro["item"]["item_id"]


def test_dynamo_only_packet_and_equipment_refusal(tmp_path):
    pipe = loot.NativeLoot(tmp_path, dynamo_drops=True)
    with pytest.raises(SchemaError, match="No owned"):
        pipe.equipment(equipment_event(pipe.session))
    item = pipe.settle(event(pipe.session))
    packet = (tmp_path / "MC3Forge-rolled-loadout.ini").read_text()
    assert "item=none\npower=0\n" in packet and "dynamo=1\n" in packet
    pipe.equipment(equipment_event(pipe.session))
    assert pipe.report()["inventory_count"] == 1
    assert (
        item["item"]["item_id"]
        in (tmp_path / "MC3Forge-rolled-loadout.ini").read_text()
    )
    with pytest.raises(SchemaError, match="conflicts"):
        pipe.equipment(equipment_event(pipe.session, equip=True))


def test_equipment_journal_recovery_and_failed_export(tmp_path, monkeypatch):
    pipe = loot.NativeLoot(tmp_path, dynamo_drops=True)
    pipe.settle(event(pipe.session))
    path = tmp_path / "MC3Forge-equipment-actions.jsonl"
    path.write_text(json.dumps(equipment_event(pipe.session)) + "\n")
    original = pipe._atomic
    monkeypatch.setattr(
        pipe,
        "_atomic",
        lambda *_: (_ for _ in ()).throw(OSError("test publication failure")),
    )
    with pytest.raises(OSError):
        pipe.poll()
    assert pipe.action_offset == 0
    assert pipe.store.state(pipe.profile)["equipped"]["nitrous"] is None
    monkeypatch.setattr(pipe, "_atomic", original)
    pipe.poll()
    assert pipe.summary["action_duplicates"] == 1
    reopened = loot.NativeLoot(tmp_path, dynamo_drops=True)
    reopened.start()
    assert reopened.report()["equipped"]["nitrous"] is None
    assert reopened.report()["inventory_count"] == 1


def test_partial_equipment_action_blocks_start_until_complete(tmp_path):
    pipe = loot.NativeLoot(tmp_path, dynamo_drops=True)
    pipe.settle(event(pipe.session))
    path = tmp_path / "MC3Forge-equipment-actions.jsonl"
    action = json.dumps(equipment_event(pipe.session))
    path.write_text(action)
    pipe.poll()
    assert pipe.action_offset == 0
    with pytest.raises(SchemaError, match="Incomplete"):
        pipe.start()
    path.write_text(action + "\n")
    pipe.start()
    assert pipe.report()["equipped"]["nitrous"] is None


def test_repeatable_rolls_equip_latest_keep_previous_and_reopen(tmp_path, monkeypatch):
    draws = iter((100, 10))
    monkeypatch.setattr(loot.secrets, "choice", lambda _: next(draws))
    pipe = loot.NativeLoot(tmp_path)
    first = pipe.settle(event(pipe.session))
    assert pipe.settle(event(pipe.session))["duplicate"]
    second = pipe.settle(event(pipe.session, 2))
    assert first["item"]["item_id"] != second["item"]["item_id"]
    assert second["percent"] == 10  # A weaker roll still becomes the equipped part.
    state = pipe.store.state(pipe.profile)
    assert state["inventory_count"] == 2
    assert state["equipped"]["engine"] == second["item"]["item_id"]
    packet = (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes()
    reopened = loot.NativeLoot(tmp_path)
    assert reopened.report()["inventory_count"] == 2
    assert (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes() == packet
    assert reopened.settle(event(pipe.session))["duplicate"]
    assert (tmp_path / "MC3Forge-rolled-loadout.ini").read_bytes() == packet


def test_committed_roll_recovers_after_export_failure_without_reroll(
    tmp_path, monkeypatch
):
    pipe = loot.NativeLoot(tmp_path)
    original = pipe._atomic
    draws = []
    monkeypatch.setattr(loot.secrets, "choice", lambda _: draws.append(25) or 25)

    def fail(*_):
        raise OSError("simulated publish failure")

    monkeypatch.setattr(pipe, "_atomic", fail)
    with pytest.raises(OSError):
        pipe.settle(event(pipe.session))
    assert pipe.store.state(pipe.profile)["inventory_count"] == 1
    monkeypatch.setattr(pipe, "_atomic", original)
    assert pipe.settle(event(pipe.session))["duplicate"]
    assert draws == [25]
    assert "power=25\n" in (tmp_path / "MC3Forge-rolled-loadout.ini").read_text()


def test_policy_and_conflicting_duplicate_do_not_make_items(tmp_path):
    pipe = loot.NativeLoot(tmp_path)
    result = pipe.settle(event(pipe.session, rank=4, max_rank=3))
    assert result["outcome"] == "ineligible"
    assert pipe.report()["inventory_count"] == 0
    with pytest.raises(SchemaError, match="conflicts"):
        pipe.settle(event(pipe.session, rank=2, max_rank=3))
    assert pipe.report()["inventory_count"] == 0
    assert pipe.settle(event(pipe.session, 2, rank=8))["outcome"] == "delivered"


@pytest.mark.parametrize(
    "change",
    [
        {"rank": True},
        {"rank": 0},
        {"sequence": 0},
        {"code_mask": 6},
        {"max_rank": 9},
        {"car": 'vp_bad"'},
        {"session": "a" * 32},
        {"schema": True},
        {"extra": 1},
    ],
)
def test_bad_events_never_change_inventory(tmp_path, change):
    pipe = loot.NativeLoot(tmp_path)
    with pytest.raises(SchemaError):
        pipe.settle(event(pipe.session) | change)
    assert pipe.report()["inventory_count"] == 0


def test_partial_event_waits_and_prior_session_recovers(tmp_path):
    pipe = loot.NativeLoot(tmp_path)
    path = tmp_path / "MC3Forge-race-results.jsonl"
    text = json.dumps(event(pipe.session))
    path.write_text(text)
    pipe.poll()
    assert pipe.offset == 0 and pipe.report()["inventory_count"] == 0
    path.write_text(text + "\n")
    reopened = loot.NativeLoot(tmp_path)
    reopened.start()
    assert reopened.report()["delivered"] == 1
    assert reopened.report()["inventory_count"] == 1
    assert (
        tmp_path / "MC3Forge-loot-session.txt"
    ).read_text().strip() == reopened.session
    reopened.poll()
    assert reopened.report()["delivered"] == 1


def test_partial_or_malformed_journal_prevents_new_session(tmp_path):
    pipe = loot.NativeLoot(tmp_path)
    path = tmp_path / "MC3Forge-race-results.jsonl"
    path.write_text('{"schema":')
    with pytest.raises(SchemaError, match="Incomplete"):
        pipe.start()
    assert not (tmp_path / "MC3Forge-loot-session.txt").exists()
    path.write_text("broken\n")
    with pytest.raises(SchemaError, match="Malformed"):
        pipe.poll()
    assert pipe.offset == 0


def test_packet_recreated_from_database_and_legacy_diagnostic_untouched(tmp_path):
    legacy = tmp_path / "MC3Forge-test-part.ini"
    legacy.write_bytes(b"preserved diagnostic")
    pipe = loot.NativeLoot(tmp_path)
    pipe.settle(event(pipe.session))
    packet = tmp_path / "MC3Forge-rolled-loadout.ini"
    original = packet.read_bytes()
    packet.unlink()
    loot.NativeLoot(tmp_path)
    assert packet.read_bytes() == original
    assert legacy.read_bytes() == b"preserved diagnostic"
