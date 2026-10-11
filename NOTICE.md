# Third-party notices

KenshiKin is a fork of [KenshiMP](https://github.com/xemou/KenshiMP). Both are free software, licensed under
the **GNU General Public License v3** (see `LICENSE`).
Copyright (C) the KenshiMP authors; the KenshiKin changes were written by an AI model (Claude, Anthropic) for
the KenshiKin project.

KenshiKin is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Lo-Fi Games,
the KenshiMP authors or Anthropic. *Kenshi* is a trademark / property of Lo-Fi Games.

The sections below are KenshiMP's notice (names updated to the fork's files).

## What the mod is built on

| Component | Role | License |
|---|---|---|
| [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) (BFrizzleFoShizzle) | Headers and import library used to call and hook the game's engine; linked into `KenshiMP.dll` | GPLv3 |
| KenshiLib.dll (the build shipped with RE_Kenshi 0.3.5) | **Redistributed unmodified** in the package, used by `KenshiMP_Loader.dll` when RE_Kenshi is not installed. Source: <https://github.com/BFrizzleFoShizzle/KenshiLib> | GPLv3 |
| [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) (BFrizzleFoShizzle) | Optional plugin loader that runs `KenshiMP.dll` (not redistributed). KenshiMP's address table for the stock Steam 1.0.68 executable (`rva/`) is derived from RE_Kenshi's 1.0.65 table by `tools/rva/make_table.ps1` | GPLv3 |
| Iced (0xd4d) | x86 disassembler used offline by `tools/rva/make_table.ps1` (not shipped) | MIT |
| Boost 1.60 (headers / static libs used by KenshiLib's API) | Compile-time dependency | Boost Software License 1.0 |
| MyGUI | In-game UI toolkit (the game's own copy is used at run time) | MIT |
| OGRE | Rendering engine API used through KenshiLib (the game's own copy is used at run time) | MIT |

Because KenshiLib is GPLv3 and is linked into the plugin, the whole of KenshiMP is distributed under
GPLv3 and **its complete corresponding source code is published** at
<https://github.com/xemou/KenshiMP>. The published source must match the released DLL
(tag the commit used for each release).

## What is NOT included in the package

- Kenshi itself, its executables, assets or game data.
- RE_Kenshi, its downgraded executable, or the PhysX DLLs.
- NVIDIA PhysX System Software (a separate NVIDIA installer; users download it themselves).
- The Microsoft Visual C++ 2010 compilers used to build the plugin (build-time only, extracted from the
  Windows SDK 7.1 for local use; never shipped).

## Package content (what Steam players receive)

`KenshiMP.dll`, `KenshiMP_Loader.dll`, `KenshiLib.dll`, `rva/RE_Kenshi/RVAs/Steam_1.0.65.br` (the
stock Steam 1.0.68 table, under the name KenshiLib looks for), `gui/core/core_settings.xml` (the
game's two interface settings plus the MyGUI plugin entry that starts the loader), `KenshiMP.mod`,
`RE_Kenshi.json`, `kenshimp.cfg` (default template),
`README.md`, `README.ru.md`, `CHANGELOG.md`, `LICENSE`, `NOTICE.md`.

## KenshiKin fork: ideas from other Kenshi co-op projects

KenshiKin (this fork) re-implemented, in its own code, approaches documented by other open Kenshi co-op
projects. No source file was copied from them.

| Project | License | What KenshiKin took (as an approach) |
|---|---|---|
| [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop) | AGPL-3.0 | body-state flags (posture, carried, bed, cage, chained), down enforcement (knock down / hold / revive), carried-body sync, bed / prison placement |
| [Melaus123/Shared-Wastelands](https://github.com/Melaus123/Shared-Wastelands) | MIT | engine hooks run on the AI worker thread; knockout / death refused on copies; crime state replicated owner -> copy through notifyCrimeWitnessed, sent on change or re-set |
| [LRaverdy/KenshiCoop](https://github.com/LRaverdy/KenshiCoop) | none stated | idea only: clients' log lines on the host, host console window, resync |

