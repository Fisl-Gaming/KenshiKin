# KenshiKin

**Co-op and PvP multiplayer for Kenshi** — a fork of [xemou/KenshiMP](https://github.com/xemou/KenshiMP) that
focuses on keeping the players' worlds in sync: sneaking, knock-outs, carrying bodies, healing, thefts, town
doors and gates, beds and cages.

[Русская версия](README.ru.md)

> **Status: beta.** Played by a group of three friends on Steam 1.0.68. Many things work, some are only
> tested offline, and you will meet bugs — see [Known issues](#known-issues). Back up your saves (the host
> does it automatically, see `backup_saves`).

## How this mod is made

**Every change KenshiKin makes on top of KenshiMP was written by an AI** — Claude (Anthropic), working
through Claude Code. No person wrote or edited the code of this fork. People did the rest: they chose what to
fix, played the test sessions, and sent the logs when something broke; the AI read the logs, the engine
headers (KenshiLib) and other open co-op mods, and wrote the fixes.

What that means for you:

- The fixes rest on what could be learned about Kenshi's engine from headers, logs and other projects, not on
  Lo-Fi Games' source code. Some of them are educated guesses that turned out right in play; some are not
  tested in play yet (marked below).
- Every risky engine call is guarded (SEH): when something does not work on your game build, that feature
  switches itself off and writes a line in the log instead of crashing the game.
- The base mod, KenshiMP, is the work of its own authors (see [NOTICE.md](NOTICE.md)).

## What you get

- **Everyone keeps their own squad, faction, base and towns.** Other players' characters appear as live
  copies: movement, combat, equipment, injuries, lost limbs, prosthetics, names above their heads.
- **The host's world is shared:** NPCs and animals around each player, time of day, weather, pause, town
  states (destroyed or taken towns, dead uniques), broken doors, town chests.
- **Player relations:** war, peace and alliance per player; direct trading in the game's trade window.
- **Steam first:** join through a Steam invitation from the title screen, no IP or port forwarding needed
  (direct IP / VPN also works).

What this fork adds or fixes compared with KenshiMP v18:

| | |
|---|---|
| Sneaking | other players see you crouch |
| Furniture and lights | towns a client loads while connected are no longer empty and dark |
| Bars and town life | NPCs sit on their stools and beds as on the host |
| Doors and gates | open / closed / locked as in the host's world; a lockpicked door opens for the host too |
| Knock-outs and deaths | only the owner decides; a copy never falls, stays down or gets up on its own |
| Carrying bodies | whoever lifts a downed player (a friend or the host's NPCs) carries the real body in the owner's world too |
| Healing | first aid and doctoring on another player's character reach the real one |
| Loot on the ground | what lies on the ground in the host's world (NPC drops, items in ruins) is the same for everybody; bags on the ground share their content; a town container whose zone the host has not loaded is loaded for you |
| Posture, beds, cages | crawling, playing dead, in a bed, in a cage, on a prisoner pole — the same everywhere |
| Thefts and crimes | stealing from a town container, lockpicking, trespassing or assault in the host's world is seen by the host's NPCs, who react; crimes between players are left to the players |
| Diagnostics | clients' log lines arrive in the host's log; host console window outside the game; RESYNC button |
| Performance | lighter per-frame work for ghosts and replicated NPCs |

## Install (every player)

1. Download `KenshiKin_vXXXX.zip` from [Releases](../../releases) and unzip it into `Kenshi\mods\`, so that
   you get `Kenshi\mods\KenshiKin\KenshiKin.dll`.
2. In the Kenshi launcher, **Mods** tab: tick **KenshiKin**, untick **KenshiMP** if you have it. They cannot run together.
3. Everybody needs the **same KenshiKin release** and **the same mod list in the same order**; the host
   refuses anything else and says what differs.

Requirements: Kenshi on Steam (1.0.68, as Steam installs it), or GOG / 1.0.65 with
[RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847). Borderless window mode is recommended (exclusive
fullscreen pauses the game on Alt+Tab).

## Play

- **Host:** title screen → **MULTIPLAYER** (or F4 in game) → **HOST**, then **STEAM FRIENDS → INVITE**.
  Start a new game or continue.
- **Friends:** accept the Steam invitation (or "Join game" on the host's profile), then **NEW GAME** (a fresh
  game is best) or **CONTINUE**. **GO TO** (or `/goto`) in the Multiplayer window brings your squad to the host.
- **Without Steam:** ADVANCED SETTINGS → host address, port (TCP 47000 by default), password.
- **Chat:** Enter. Commands: `/players`, `/war`, `/peace`, `/ally <player|all>`, `/goto [player]`,
  `/resync [all|player]`, `/console` (host), `/report`.
- **Host console:** a window outside the game (minimised in the taskbar when you host) with the players, the
  live log of everyone and commands (`players`, `resync all`, `resync <name>`, `say <text>`).

Client saves go to `<name>_MP`; your own single-player saves are never touched.
Settings: `%LOCALAPPDATA%\kenshi\KenshiKin\kenshikin.cfg` (every option is explained in the file).

## Known issues

Things you may run into, most likely first:

- **Each player still has their own world.** Only what is listed above is shared. Anything else can differ
  between players (for example the contents of containers in your own base, or what a client's own
  simulation does far from the host with `load_sharing=1`).
- **A client's own ground loot is replaced by the host's.** Kenshi rolls loot at random in each world, so
  around a client's characters the items lying on the ground in its own world are removed and the host's
  are shown (`world_ground=1`). Items near the client's own buildings and anything it dropped itself are
  never removed.
- **Different mods = different worlds.** A missing furniture or item mod on one player's side shows up as
  missing chairs, NPCs standing instead of sitting, or items that cannot be created. Use the same mod list.
- **Guards react to a theft after a delay** (10–20 s in our tests): the crime is set at once, but the
  host's NPCs still have to notice it and come.
- **Guards do not recognise a wanted friend on sight.** Kenshi only checks bounties of the player faction;
  other players' characters live in their own factions. Crimes caught in the act work.
- **Not tested in play yet:** beds and cages, posture (crawling, playing dead), healing another player,
  lockpicking reports, the host console window. They are built and tested offline; please report what you see.
- **Ranged combat:** a copy fires its own arrow when its owner fires, so the arrow's path can differ
  (the damage is always the owner's).
- **Internet play over TCP:** on a bad connection, characters can stutter or catch up in jumps. Steam or a
  gaming VPN (Radmin, ZeroTier, Tailscale) helps.
- **The host leaving ends the session** for everybody (no host migration).
- **Fast-forward is locked to x1** while connected.
- Up to 8 players; tested with 3.

When something breaks: the host's `Kenshi\RE_Kenshi_log.txt` now holds everybody's lines (`[name] ...`).
Open an issue with that file and a short description of what you saw.

## Build from source

The plugin must be built with the **Visual C++ 2010 x64** toolchain (KenshiLib requires it). Put these next
to the repository, in `..\deps\`:

| Folder | What |
|---|---|
| `deps\vc2010\` | the VC++ 2010 x64 compilers and the Windows SDK 7.1, extracted with `msiexec /a` from the Windows SDK 7.1 ISO (`GRMSDKX_EN_DVD.iso`: `Setup\vc_stdamd64`, `Setup\vc_stdx86`, `Setup\WinSDKBuild_amd64`) — nothing is installed |
| `deps\KenshiLib\` | [KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) headers |
| `deps\KenshiLib_Examples_deps\` | [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps) (git LFS: libs, Boost 1.60 — unzip `boost.zip`) |
| `deps\KenshiLib_Examples\` | [KenshiLib_Examples](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples) (the empty `HelloWorld.mod`) |
| `deps\re_release\install\KenshiLib.dll` | the KenshiLib.dll built with RE_Kenshi 0.3.5 |

Then `plugin\build.bat` builds the DLLs and `package.bat` assembles `dist\KenshiKin\`.
`tests\build_and_run.bat` (or the same `cl` line with the VC2010 toolchain) runs the offline network tests.

## License and credits

GPL-3.0, like KenshiMP and KenshiLib (see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md)).

- [xemou/KenshiMP](https://github.com/xemou/KenshiMP) — the mod this fork is built on; its original
  documentation is in [docs/upstream-kenshimp](docs/upstream-kenshimp).
- [KenshiLib / RE_Kenshi](https://github.com/BFrizzleFoShizzle) — engine access.
- Approaches learned from [nhoral/KenshiCoop](https://github.com/nhoral/KenshiCoop),
  [Melaus123/Shared-Wastelands](https://github.com/Melaus123/Shared-Wastelands) and
  [LRaverdy/KenshiCoop](https://github.com/LRaverdy/KenshiCoop) (re-implemented, no code copied).

KenshiKin is an unofficial fan project, not affiliated with Lo-Fi Games, with the KenshiMP authors, or with
Anthropic. *Kenshi* is a trademark of Lo-Fi Games.
