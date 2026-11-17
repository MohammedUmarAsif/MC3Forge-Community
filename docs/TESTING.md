# Testing and evidence

## Source-only checks

```powershell
uv sync --locked
uv run pytest
uv run ruff check .
uv run ruff format --check .
uv run ty check runtime tools
```

Native protocol check, without a game:

```sh
c++ -std=c++17 -O2 -I native tests/native_protocol.cpp -o native-protocol-test
./native-protocol-test
```

Use a temporary output folder on Windows and the equivalent MSVC command if preferred. These fixtures are synthetic. Public CI never downloads a game, BIOS or VIP files.

## Human checks still needed

Use disposable copies and ordinary Continue; no savestates. Open equipment only in free roam. Check slot browsing, current/candidate comparison, explicit Equip and Unequip, Triangle affix tuning, Keep/Apply, Select help, Circle Back and Start closure. Check controller input does not also drive/pause the guest and releases correctly on close/disconnect. Cold restart should retain the intended loadout and pending offer.

The optional DEV pack is deliberately extreme. Try a couple of pieces first, then all eight. Other equipped parts continue contributing when one is removed; earned nitro bottles are not removed by unequipping. Rarity/quantity affect future rewards rather than cruise handling. The ten-grants-per-launch diagnostic guard remains.

For money, equip money-bearing parts before a supported paid race and leave Results visible briefly. With all eight DEV parts, expected bonus=2x native base and wallet increase=3x base. Example:700 base +1400 bonus =2100 earned. Original game Results text may still show700; use the wallet and trainer MONEY HUD. OFF: remove all money affixes before another paid race; compare that result's own base and expect bonus0. Different events/positions can have different prizes.

Report the exact refusal if no credit occurs. Do not repeatedly race to force a write. Base/bonus, wallet before/native-after/final-after, source mask and prepared/applied stages are in local status/receipts. Source mask127 only confirms matching code, not a successful money test. A failed applied-receipt write can follow a successful wallet write; never manually replay an unresolved credit.

## What has been observed

Local copied-profile tests passed earlier race-to-loot, ordinary power application, additive +100/+105/+110% and restart retention. New menu/controller flow, extreme stacking and generalized current-trainer money remain pending human tests. Automated native money tests cover percentage arithmetic, delayed settlement, duplicate Results/late attachment, journal failures, source/owner/wallet/rate/reset/overflow refusals. Local startup accepted the14-item dev inventory and matched payout code; booting minted no money credits. The public clean-upstream install has not been gameplay-qualified.
