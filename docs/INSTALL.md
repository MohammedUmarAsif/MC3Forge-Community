# Installation: choose the correct route

## Ordinary players

This first release is a source/developer preview. There is no verified copy-and-paste player package yet. **Do not copy the source into a stock PCSX2 folder and expect equipment to appear.** Keep your working VIP installation unchanged until a reviewed standalone runtime is supplied or you build one.

You supply your own compatible PS2 Remix + VIP 1.1-A image, legally obtained PS2 BIOS and a memory-card copy. No game, VIP pack, BIOS or save is supplied. This addon is not for Xbox builds, xrevive, native PC ports, CHD input in this helper, or arbitrary mod combinations.

## With a compatible custom runtime

1. Install Python 3.12 or newer. Extract this source ZIP to a writable folder, for example `D:\MC3Forge`. Keep it outside your existing game/emulator directories.
2. Build the pinned runtime using [BUILD.md](BUILD.md), or use a separately reviewed compatible runtime. A stock `pcsx2-qt.exe` will be refused by the helper.
3. Open a terminal in the extracted folder and run:

```powershell
py -3 tools/player.py setup --emulator "D:\CustomPCSX2\bin" --iso "D:\MyGames\MC3-VIP.iso" --bios "D:\MyBIOS\my-bios.bin" --card "D:\MySaves\my-card.ps2"
```

The helper creates `.local/play`, copies the selected BIOS/card and an allowlisted runtime, and writes a clean portable configuration. Your originals remain untouched. It refuses an existing destination instead of overwriting a save. Configure your controller in this isolated emulator if its default mapping is unsuitable.

4. Start the isolated experimental build:

```powershell
py -3 tools/player.py run --experimental
```

Continue normally into free roam, then press tilde. The terminal owns the companion while the game runs; keep it open. Close the game normally. The explicit `--experimental` flag acknowledges gameplay and wallet writes in the copied profile. This is not a verified fresh-PC installation: the new clean-upstream integration and portable helper still need a complete clean-machine gameplay pass.

5. Optional DEV pack, with the game closed:

```powershell
py -3 tools/dev_pack.py
```

It adds eight unequipped DEV parts once. Use the equipment menu to equip them. Normal play does not automatically grant this pack.

## Where data goes / backup / uninstall

The ISO is read from your chosen path; the BIOS/card copies, SQLite inventory, journals and settings stay in `.local/play`. Back up that whole folder **with the game closed**. Game cash is saved by the game's ordinary save flow; inventory uses SQLite separately. Do not assume copying just one file preserves both.

To uninstall, close the game/terminal, back up `.local/play`, then remove the MC3Forge folder yourself. No original emulator, original card, Windows setting, driver or system-wide service is modified by these helpers. No administrator rights are needed. Do not upload `.local` or raw logs when reporting bugs.
