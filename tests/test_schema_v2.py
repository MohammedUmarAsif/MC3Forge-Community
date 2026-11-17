"""Confirmed rules, including memberships consuming ordinary property capacity."""

import pytest
from lootlab.schema_v2 import EQUIPMENT_SLOTS, Affix, Item, SchemaError


def item(rarity="common", ordinary=1, memberships=(), **kwargs):
    return Item(
        "part-1",
        "engine",
        rarity,
        tuple(Affix(f"stat-{i}", 2) for i in range(ordinary)),
        memberships,
        **kwargs,
    )


@pytest.mark.parametrize(
    "rarity,n,sets,stage",
    [
        ("common", 1, (), "career"),
        ("uncommon", 2, (), "career"),
        ("rare", 3, (), "career"),
        ("epic", 4, (), "career"),
        ("epic", 3, ("roy",), "career"),
        ("divine", 3, ("roy",), "ngplus"),
        ("ethereal", 4, ("roy",), "ngplus"),
        ("ethereal", 6, ("roy", "angel"), "ngplus"),
    ],
)
def test_confirmed_budgets_round_trip(rarity, n, sets, stage):
    part = item(rarity, n, sets, progression_at_creation=stage)
    assert Item.from_dict(part.to_dict()) == part
    assert part.property_count == n + len(sets)


@pytest.mark.parametrize(
    "rarity,n,sets,stage",
    [
        ("common", 1, ("roy",), "career"),
        ("rare", 2, ("roy",), "career"),
        ("epic", 4, ("roy",), "career"),
        ("divine", 4, ("roy",), "ngplus"),
        ("divine", 3, ("roy",), "career"),
        ("ethereal", 3, ("roy",), "ngplus"),
        ("ethereal", 7, ("roy", "angel"), "ngplus"),
        ("ethereal", 3, ("roy", "roy"), "ngplus"),
    ],
)
def test_old_lab_rules_cannot_sneak_into_v2(rarity, n, sets, stage):
    with pytest.raises(SchemaError):
        item(rarity, n, sets, progression_at_creation=stage)


def test_accessory_fits_either_slot_but_never_a_component():
    a = Item("charm", "accessory", "common", (Affix("power", 2),))
    assert len(EQUIPMENT_SLOTS) == 8
    assert a.fits("accessory_1") and a.fits("accessory_2") and not a.fits("engine")
    assert not item().fits("accessory_1")


def test_money_duplicate_rejected_without_inventing_other_conflict_groups():
    with pytest.raises(SchemaError, match="money"):
        Item("x", "engine", "uncommon", (Affix("money", 2), Affix("money", 3)))
    Item("x", "engine", "uncommon", (Affix("power", 2), Affix("power", 3)))


@pytest.mark.parametrize("value", [float("nan"), float("inf"), True])
def test_non_numeric_rolls_do_not_enter_saved_data(value):
    with pytest.raises(SchemaError):
        Affix("power", value)


def test_no_common_or_uncommon_reroll_or_undeclared_schema_fields():
    assert item().reroll_forbidden and item("uncommon", 2).reroll_forbidden
    assert not item("rare", 3).reroll_forbidden
    value = item().to_dict() | {"ability_wheel": True}
    with pytest.raises(SchemaError):
        Item.from_dict(value)
