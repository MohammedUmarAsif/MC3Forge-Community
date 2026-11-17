# Contributing

Read README, docs/ARCHITECTURE.md, docs/TESTING.md and SECURITY.md first. Keep changes small and evidence-backed. Preserve save formats, operation identities and unrelated work.

Use Python3.12+ and `uv sync --locked`, then `uv run pytest`, `uv run ruff check .`, `uv run ruff format --check .` and `uv run ty check runtime tools`. Native protocol tests need a C++17 compiler; runtime integration uses PCSX2's C++20 toolchain. Never require a game/BIOS input in public CI.

Keep policy in Python, persistence transactional, render state immutable and gameplay writes on the CPU thread. Add a regression for a reproduced bug. Include what changed, why, validation and remaining uncertainty. Benchmark performance claims; do not remove guards simply to shorten code.

No game/mod assets, saves, decompiled exports, generated guard bytes, credentials or private evidence in commits, issues or pull requests. Preserve third-party credits and compatible licenses. Contributors license their original contributions under GPL-3.0-or-later.
