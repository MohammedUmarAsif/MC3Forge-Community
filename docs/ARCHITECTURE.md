# Architecture

One inventory authority, one native gameplay owner, and a renderer that emits requests.

1. `runtime/native_inventory.py`: affix pools, reward weights/pity, additive build math and per-action rules.
2. `runtime/native_loot.py` + `runtime/lootlab/store_v2.py`: durable operation receipts, SQLite transactions, journal consumption and atomic projections. SQLite is authoritative; exported text is a validated snapshot, not a second database.
3. `native/InventoryWire.h` and `LootWire.h`: bounded typed snapshots/actions. Matching revisions prevent acting on mismatched inventory/loadout publications.
4. `native/EquipmentMenu.h`: navigation state and replacement arithmetic. `EquipmentUI.h`: Dear ImGui presentation from immutable snapshots; no guest memory or filesystem access in rendering.
5. `native/MC3ForgeTrainer.cpp`: CPU integration, input packet gate and thread-safe publication. `TrainerCore.h`, `Dynamo.h`, `Money.h`: guarded gameplay effects. Writes stay on the emulation CPU thread.
6. `tools/player.py`: isolated portable profile and companion lifecycle. It does not expose an HTTP server or PINE endpoint.

Do not collapse ownership guards, transactions or recovery into UI callbacks to make code shorter. Public source uses a small runtime/tools/native/tests/docs layout; private research paths and historical experiments are intentionally excluded.

Money uses a pre-finish wallet baseline, observed native paid finish, pinned code guards, an intent before the extra wallet write and readback/applied receipt afterward. A failed/ambiguous write is never automatically retried. This is not an atomic transaction spanning SQLite and the game, and not a guarantee of recovery from power loss.
