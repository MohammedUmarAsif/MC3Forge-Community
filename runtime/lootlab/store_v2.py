"""Durable eight-slot study storage; namespaces are not yet native save identities."""

from __future__ import annotations

import hashlib
import json
import sqlite3
import uuid
from collections.abc import Callable, Generator
from contextlib import closing, contextmanager
from pathlib import Path

from .schema_v2 import EQUIPMENT_SLOTS, SCHEMA_VERSION, Item, SchemaError, identifier

APPLICATION_ID = 0x4D433346
REQUIRED_TABLES = {
    "profiles_v2",
    "items_v2",
    "loadouts_v2",
    "equipment_v2",
    "journal_v2",
}


def canonical(value: dict) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def label(value: str) -> str:
    if (
        not isinstance(value, str)
        or not value.strip()
        or len(value) > 128
        or "\0" in value
    ):
        raise SchemaError("Choose a nonempty profile/loadout label up to128 characters")
    return value


class EquipmentStoreV2:
    STORAGE_VERSION = 2

    def __init__(self, path: Path):
        self.path = path.resolve(strict=True)
        # Inspect first through a read-only connection. Never upgrade legacy DBs
        # merely because a user points this class at one.
        with closing(sqlite3.connect(self.path.as_uri() + "?mode=ro", uri=True)) as db:
            if (
                db.execute("PRAGMA application_id").fetchone()[0] != APPLICATION_ID
                or db.execute("PRAGMA user_version").fetchone()[0]
                != self.STORAGE_VERSION
                or not REQUIRED_TABLES
                <= {
                    r[0]
                    for r in db.execute(
                        "SELECT name FROM sqlite_master WHERE type='table'"
                    )
                }
            ):
                raise SchemaError(
                    f"Not a storage-v{self.STORAGE_VERSION} equipment DB; explicit copy migration is required"
                )

    @classmethod
    def create(cls, path: Path) -> EquipmentStoreV2:
        path = path.resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb"):
            pass
        with closing(sqlite3.connect(path)) as db:
            db.executescript("""
                PRAGMA foreign_keys=ON;
                PRAGMA application_id=0x4D433346;
                PRAGMA user_version=2;
                CREATE TABLE profiles_v2 (
                    profile_id TEXT PRIMARY KEY, label TEXT NOT NULL,
                    revision INTEGER NOT NULL DEFAULT 0 CHECK(revision>=0),
                    active_loadout TEXT NOT NULL DEFAULT 'main',
                    native_binding_status TEXT NOT NULL DEFAULT 'unbound'
                        CHECK(native_binding_status='unbound'),
                    FOREIGN KEY(profile_id,active_loadout)
                        REFERENCES loadouts_v2(profile_id,loadout_id)
                        DEFERRABLE INITIALLY DEFERRED);
                CREATE TABLE loadouts_v2 (
                    profile_id TEXT NOT NULL, loadout_id TEXT NOT NULL, label TEXT NOT NULL,
                    PRIMARY KEY(profile_id,loadout_id),
                    FOREIGN KEY(profile_id) REFERENCES profiles_v2(profile_id));
                CREATE TABLE items_v2 (
                    profile_id TEXT NOT NULL, item_id TEXT NOT NULL, data TEXT NOT NULL,
                    data_kind TEXT NOT NULL DEFAULT 'design_study' CHECK(data_kind='design_study'),
                    PRIMARY KEY(profile_id,item_id),
                    FOREIGN KEY(profile_id) REFERENCES profiles_v2(profile_id));
                CREATE TABLE equipment_v2 (
                    profile_id TEXT NOT NULL, loadout_id TEXT NOT NULL,
                    slot TEXT NOT NULL CHECK(slot IN ('engine','nitrous','tires','brakes',
                        'transmission','suspension','accessory_1','accessory_2')),
                    item_id TEXT NOT NULL, PRIMARY KEY(profile_id,loadout_id,slot),
                    UNIQUE(profile_id,loadout_id,item_id),
                    FOREIGN KEY(profile_id,loadout_id) REFERENCES loadouts_v2(profile_id,loadout_id),
                    FOREIGN KEY(profile_id,item_id) REFERENCES items_v2(profile_id,item_id));
                CREATE TABLE journal_v2 (
                    profile_id TEXT NOT NULL, operation_id TEXT NOT NULL,
                    payload_sha256 TEXT NOT NULL, receipt TEXT NOT NULL,
                    created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                    PRIMARY KEY(profile_id,operation_id),
                    FOREIGN KEY(profile_id) REFERENCES profiles_v2(profile_id));
            """)
        return cls(path)

    @contextmanager
    def connection(self, *, write: bool = False) -> Generator[sqlite3.Connection]:
        with closing(
            sqlite3.connect(self.path.as_uri() + "?mode=rw", uri=True, timeout=5)
        ) as db:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA foreign_keys=ON")
            db.execute("BEGIN IMMEDIATE" if write else "BEGIN")
            try:
                yield db
                db.commit()
            except BaseException:
                db.rollback()
                raise

    def create_profile(self, name: str) -> str:
        profile_id = uuid.uuid4().hex
        with self.connection(write=True) as db:
            db.execute(
                "INSERT INTO profiles_v2(profile_id,label) VALUES (?,?)",
                (profile_id, label(name)),
            )
            db.execute(
                "INSERT INTO loadouts_v2 VALUES (?,?,?)", (profile_id, "main", "Main")
            )
        return profile_id

    def _profile(self, db: sqlite3.Connection, profile: str) -> sqlite3.Row:
        row = db.execute(
            "SELECT * FROM profiles_v2 WHERE profile_id=?", (identifier(profile),)
        ).fetchone()
        if row is None:
            raise SchemaError("Unknown profile namespace")
        return row

    def _item(self, db: sqlite3.Connection, profile: str, item_id: str) -> Item:
        row = db.execute(
            "SELECT data FROM items_v2 WHERE profile_id=? AND item_id=?",
            (profile, identifier(item_id)),
        ).fetchone()
        if row is None:
            raise SchemaError("Item is not owned by this profile namespace")
        return Item.from_dict(json.loads(row[0]))

    def _transact(
        self,
        profile: str,
        operation: str,
        payload: dict,
        mutate: Callable[[sqlite3.Connection], dict],
    ) -> dict:
        identifier(operation)
        digest = hashlib.sha256(canonical(payload).encode()).hexdigest()
        with self.connection(write=True) as db:
            self._profile(db, profile)
            prior = db.execute(
                "SELECT payload_sha256,receipt FROM journal_v2 WHERE profile_id=? AND operation_id=?",
                (profile, operation),
            ).fetchone()
            if prior:
                if prior[0] != digest:
                    raise SchemaError(
                        "Operation identity conflicts with its previous payload"
                    )
                return json.loads(prior[1]) | {"duplicate": True}
            try:
                result = mutate(db)
                db.execute(
                    "UPDATE profiles_v2 SET revision=revision+1 WHERE profile_id=?",
                    (profile,),
                )
                receipt = result | {
                    "profile_id": profile,
                    "operation_id": operation,
                    "revision": self._profile(db, profile)["revision"],
                    "duplicate": False,
                    "data_kind": "design_study",
                    "native_cash_written": False,
                    "game_connected": False,
                }
                db.execute(
                    "INSERT INTO journal_v2(profile_id,operation_id,payload_sha256,receipt) VALUES (?,?,?,?)",
                    (profile, operation, digest, canonical(receipt)),
                )
                return receipt
            except sqlite3.IntegrityError as error:
                raise SchemaError(
                    "Storage invariant rejected this operation"
                ) from error

    def add_study_item(self, profile: str, operation: str, item: Item) -> dict:
        def mutate(db: sqlite3.Connection) -> dict:
            db.execute(
                "INSERT INTO items_v2(profile_id,item_id,data) VALUES (?,?,?)",
                (profile, item.item_id, canonical(item.to_dict())),
            )
            return {"kind": "add_study_item", "item": item.to_dict()}

        return self._transact(
            profile,
            operation,
            {"kind": "add_study_item", "item": item.to_dict()},
            mutate,
        )

    def create_loadout(
        self, profile: str, operation: str, loadout: str, name: str
    ) -> dict:
        identifier(loadout)
        label(name)

        def mutate(db: sqlite3.Connection) -> dict:
            db.execute(
                "INSERT INTO loadouts_v2 VALUES (?,?,?)", (profile, loadout, name)
            )
            return {"kind": "create_loadout", "loadout_id": loadout}

        return self._transact(
            profile,
            operation,
            {"kind": "create_loadout", "loadout": loadout, "label": name},
            mutate,
        )

    def equip(
        self,
        profile: str,
        operation: str,
        loadout: str,
        slot: str,
        item_id: str | None,
        *,
        expected_revision: int | None = None,
    ) -> dict:
        identifier(loadout)
        if slot not in EQUIPMENT_SLOTS:
            raise SchemaError("Unknown equipment slot")
        if expected_revision is not None and (
            type(expected_revision) is not int or expected_revision < 0
        ):
            raise SchemaError("Expected revision must be a nonnegative integer")

        def mutate(db: sqlite3.Connection) -> dict:
            if (
                expected_revision is not None
                and self._profile(db, profile)["revision"] != expected_revision
            ):
                raise SchemaError("Profile revision changed before equipment commit")
            if (
                db.execute(
                    "SELECT 1 FROM loadouts_v2 WHERE profile_id=? AND loadout_id=?",
                    (profile, loadout),
                ).fetchone()
                is None
            ):
                raise SchemaError("Loadout is not owned by this profile namespace")
            if item_id is not None:
                part = self._item(db, profile, item_id)
                if not part.fits(slot):
                    raise SchemaError("Part does not fit this slot")
                # Moving one accessory never lets it count in both slots.
                db.execute(
                    "DELETE FROM equipment_v2 WHERE profile_id=? AND loadout_id=? AND item_id=?",
                    (profile, loadout, item_id),
                )
            db.execute(
                "DELETE FROM equipment_v2 WHERE profile_id=? AND loadout_id=? AND slot=?",
                (profile, loadout, slot),
            )
            if item_id is not None:
                db.execute(
                    "INSERT INTO equipment_v2 VALUES (?,?,?,?)",
                    (profile, loadout, slot, item_id),
                )
            return {
                "kind": "equip" if item_id else "unequip",
                "loadout_id": loadout,
                "slot": slot,
                "item_id": item_id,
            }

        payload = {
            "kind": "equip",
            "loadout": loadout,
            "slot": slot,
            "item_id": item_id,
        }
        if expected_revision is not None:
            payload["expected_revision"] = expected_revision
        return self._transact(profile, operation, payload, mutate)

    def activate_loadout(self, profile: str, operation: str, loadout: str) -> dict:
        identifier(loadout)

        def mutate(db: sqlite3.Connection) -> dict:
            if (
                db.execute(
                    "SELECT 1 FROM loadouts_v2 WHERE profile_id=? AND loadout_id=?",
                    (profile, loadout),
                ).fetchone()
                is None
            ):
                raise SchemaError("Loadout is not owned by this profile namespace")
            db.execute(
                "UPDATE profiles_v2 SET active_loadout=? WHERE profile_id=?",
                (loadout, profile),
            )
            return {"kind": "activate_loadout", "loadout_id": loadout}

        return self._transact(
            profile, operation, {"kind": "activate_loadout", "loadout": loadout}, mutate
        )

    def state(
        self,
        profile: str,
        loadout: str | None = None,
        *,
        offset: int = 0,
        limit: int = 60,
    ) -> dict:
        if (
            type(offset) is not int
            or offset < 0
            or type(limit) is not int
            or not 1 <= limit <= 200
        ):
            raise SchemaError("Use a nonnegative offset and page size1..200")
        with self.connection() as db:
            data = dict(self._profile(db, profile))
            loadout = data["active_loadout"] if loadout is None else identifier(loadout)
            if (
                db.execute(
                    "SELECT 1 FROM loadouts_v2 WHERE profile_id=? AND loadout_id=?",
                    (profile, loadout),
                ).fetchone()
                is None
            ):
                raise SchemaError("Loadout is not owned by this profile namespace")
            inventory_rows, inventory_count = self._read_inventory(
                db, profile, limit, offset
            )
            items = [Item.from_dict(json.loads(r[0])).to_dict() for r in inventory_rows]
            slots = dict.fromkeys(EQUIPMENT_SLOTS)
            equipped_items = {}
            for row in db.execute(
                "SELECT e.slot,e.item_id,i.data FROM equipment_v2 e JOIN items_v2 i ON i.profile_id=e.profile_id AND i.item_id=e.item_id WHERE e.profile_id=? AND e.loadout_id=?",
                (profile, loadout),
            ):
                part = Item.from_dict(json.loads(row[2]))
                if not part.fits(row[0]):
                    raise SchemaError("Stored item no longer fits its slot")
                slots[row[0]], equipped_items[row[0]] = row[1], part.to_dict()
            result = {
                "schema": SCHEMA_VERSION,
                "profile": data,
                "loadout_id": loadout,
                "inventory": items,
                "equipped": slots,
                "equipped_items": equipped_items,
                "inventory_count": inventory_count,
                "page": {"offset": offset, "limit": limit},
                "game_connected": False,
                "data_kind": "design_study",
                "native_profile_bound": False,
            }
            return self._finish_state(db, profile, result)

    def _read_inventory(
        self, db: sqlite3.Connection, profile: str, limit: int, offset: int
    ) -> tuple[list[sqlite3.Row], int]:
        rows = db.execute(
            "SELECT data FROM items_v2 WHERE profile_id=? ORDER BY item_id LIMIT ? OFFSET ?",
            (profile, limit, offset),
        ).fetchall()
        count = db.execute(
            "SELECT COUNT(*) FROM items_v2 WHERE profile_id=?", (profile,)
        ).fetchone()[0]
        return rows, count

    def _finish_state(self, db: sqlite3.Connection, profile: str, result: dict) -> dict:
        """Extend a view within its existing read transaction, never another snapshot."""
        return result
