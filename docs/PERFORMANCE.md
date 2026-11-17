# Performance and known limits

No minimum-PC or FPS claim has been measured. PCSX2 rendering settings often dominate cost; start at native/1x resolution and the emulator's supported default renderer before increasing resolution.

Audit changes:

- Inventory duplicate IDs use a bounded hash set rather than pairwise comparisons. Inventory remains capped at 1000 items / 1 MiB projection.
- Dynamo retains a full-RAM alias search before granting a charge. A libc candidate-byte search skips most nonmatching addresses before typed checks. It is still a whole-memory scan at grant time, not free work.
- Native status JSON is written to a temporary file and replaced atomically, so readers do not parse half a report.
- Inventory rendering clips long lists. Projections are immutable; renderer does not read SQLite or disk.
- Companion polls bounded local journals at 10 Hz. Gameplay is not Python-per-frame. Telemetry is bounded and optional; no screen capture is required.

Remaining costs: native RAM-mapping and source-body checks each tick, full alias scan at grants, repeated build-total calculation while the menu is open, and native diagnostic HUD/trace output. Do not remove checks or claim negligible overhead without measuring menu-open/closed frame times, CPU, memory and I/O on representative hardware.

Limits: one supported local player and pinned VIP code layout; no multiplayer certification; no savestate continuity; no clean-machine player binary qualification; no all-car/high-stack guarantee. New controller/navigation, advanced Dynamo and generalized money paths still need human observations. Unsupported contexts refuse rather than guessing an address or prize.
