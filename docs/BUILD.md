# Build the custom runtime

The release contains original addon source, not the private research emulator. Public runtime integration targets clean upstream PCSX2 commit `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`. Source/ELF/hook preflight and actual integration passed on a fresh checkout of that pin. The local research build passed; **a complete clean-upstream build/gameplay run remains a release gate**. Do not describe this as a verified layperson installer.

Prerequisites: Git, Python3.12+, a Windows C++20 toolchain and PCSX2's matching build dependencies. Follow [PCSX2's own build instructions](https://pcsx2.net/docs/development/building/) for dependency versions and platform requirements. MC3Forge does not silently install system-wide dependencies or drivers.

```powershell
git clone https://github.com/PCSX2/pcsx2.git .local/pcsx2-source
git -C .local/pcsx2-source checkout fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3
py -3 tools/integrate_pcsx2.py --pcsx2-source .local/pcsx2-source --vip-elf "D:\YourVIPBuilder\SLUS_213.55" --check-only
py -3 tools/integrate_pcsx2.py --pcsx2-source .local/pcsx2-source --vip-elf "D:\YourVIPBuilder\SLUS_213.55"
```

The ELF must be your own VIP1.1-A executable, SHA256 `ad22a30c5437e1fbb997e49ff7b3cd5bd309c37f69ff2075be32ccb05e15e45c`. Unsupported inputs refuse. The integration modifies four upstream files and copies the original addon sources. It generates byte guards locally from your input. Those generated headers must not be committed or uploaded. This script intentionally refuses modified targets/repeated integration; use another fresh checkout to retry.

After configuring upstream's documented dependencies/toolchain, the build follows the usual out-of-tree CMake pattern:

```powershell
cmake -S .local/pcsx2-source -B .local/pcsx2-build -DCMAKE_BUILD_TYPE=Release
cmake --build .local/pcsx2-build --config Release --target pcsx2-qt --parallel 2
```

This generic command does not fetch or configure PCSX2's external dependencies for you. Use the matching upstream dependency guide and platform generator/toolchain options. Two build workers are a conservative starting point on memory-limited PCs; runtime speed is separate from build parallelism.

Use the resulting runtime folder containing `pcsx2-qt.exe`, its DLLs and resources with [INSTALL.md](INSTALL.md). Do not publish a compiled runtime without reviewing all embedded material, corresponding source and third-party notices. The current first public release deliberately does not distribute that binary.
