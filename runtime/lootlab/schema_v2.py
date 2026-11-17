"""Confirmed property budgets and eight-slot item shapes; no native writes."""

from __future__ import annotations

import math
import re
from dataclasses import asdict, dataclass

SCHEMA_VERSION = 2
COMPONENTS = ("engine", "nitrous", "tires", "brakes", "transmission", "suspension")
ACCESSORIES = ("accessory_1", "accessory_2")
EQUIPMENT_SLOTS = (*COMPONENTS, *ACCESSORIES)
RARITIES = ("common", "uncommon", "rare", "epic", "divine", "ethereal")


class SchemaError(ValueError):
    """Rejected data must not change inventory or equipment."""


def identifier(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}", value
    ):
        raise SchemaError("Expected a short stable identifier")
    return value


@dataclass(frozen=True, slots=True)
class Affix:
    key: str
    value: float

    def __post_init__(self) -> None:
        identifier(self.key)
        if (
            isinstance(self.value, bool)
            or not isinstance(self.value, (int, float))
            or not math.isfinite(self.value)
        ):
            raise SchemaError("An affix needs a finite numeric value")
        object.__setattr__(self, "value", float(self.value))


@dataclass(frozen=True, slots=True)
class Item:
    item_id: str
    kind: str
    rarity: str
    affixes: tuple[Affix, ...]
    memberships: tuple[str, ...] = ()
    progression_at_creation: str = "career"
    level: int = 1
    source_ref: str = "design-study"

    def __post_init__(self) -> None:
        identifier(self.item_id)
        if self.kind not in (*COMPONENTS, "accessory") or self.rarity not in RARITIES:
            raise SchemaError("Unknown item kind or rarity")
        if type(self.level) is not int or self.level < 1:
            raise SchemaError(
                "Item level must be a positive integer; balance range is open"
            )
        if self.progression_at_creation not in ("career", "ngplus"):
            raise SchemaError("Unknown creation stage")
        if (
            not isinstance(self.source_ref, str)
            or not 1 <= len(self.source_ref) <= 512
            or "\0" in self.source_ref
        ):
            raise SchemaError("An explicit source reference is required")
        if not isinstance(self.affixes, tuple) or not all(
            isinstance(a, Affix) for a in self.affixes
        ):
            raise SchemaError("Affixes must be an immutable tuple of typed rolls")
        if not isinstance(self.memberships, tuple):
            raise SchemaError("Memberships must be an immutable tuple")
        for name in self.memberships:
            identifier(name)
        if len(set(self.memberships)) != len(self.memberships):
            raise SchemaError("An item cannot count twice toward the same set")
        if sum(a.key == "money" for a in self.affixes) > 1:
            raise SchemaError("At most one ordinary money affix per item")
        ordinary, memberships = len(self.affixes), len(self.memberships)
        total = ordinary + memberships
        valid = {
            "common": ordinary == 1 and memberships == 0,
            "uncommon": ordinary == 2 and memberships == 0,
            "rare": ordinary == 3 and memberships == 0,
            # Preserve legacy four-property parts; new six-affix prototype parts have no sets yet.
            "epic": (total == 4 and memberships in (0, 1))
            or (ordinary == 6 and memberships == 0),
            "divine": ordinary == 3 and memberships == 1,
            "ethereal": 5 <= total <= 8 and memberships in (1, 2),
        }[self.rarity]
        if not valid:
            raise SchemaError("Properties violate the confirmed rarity budget")
        if (
            self.rarity in ("divine", "ethereal")
            and self.progression_at_creation != "ngplus"
        ):
            raise SchemaError("Divine/Ethereal creation requires NG+")

    @property
    def property_count(self) -> int:
        return len(self.affixes) + len(self.memberships)

    @property
    def reroll_forbidden(self) -> bool:
        # False is not permission: fees/access/membership policy remain open.
        return self.rarity in ("common", "uncommon")

    def fits(self, slot: str) -> bool:
        return (self.kind == "accessory" and slot in ACCESSORIES) or (
            self.kind in COMPONENTS and slot == self.kind
        )

    def to_dict(self) -> dict:
        return {
            "schema": SCHEMA_VERSION,
            **asdict(self),
            "affixes": [asdict(a) for a in self.affixes],
            "memberships": list(self.memberships),
        }

    @classmethod
    def from_dict(cls, value: dict) -> Item:
        expected = {
            "schema",
            "item_id",
            "kind",
            "rarity",
            "affixes",
            "memberships",
            "progression_at_creation",
            "level",
            "source_ref",
        }
        if set(value) != expected or value["schema"] != SCHEMA_VERSION:
            raise SchemaError("Unsupported item schema or undeclared properties")
        try:
            return cls(
                item_id=value["item_id"],
                kind=value["kind"],
                rarity=value["rarity"],
                affixes=tuple(Affix(**a) for a in value["affixes"]),
                memberships=tuple(value["memberships"]),
                progression_at_creation=value["progression_at_creation"],
                level=value["level"],
                source_ref=value["source_ref"],
            )
        except (TypeError, KeyError) as error:
            raise SchemaError("Malformed v2 item") from error
