# NBA LIVE 19 — native PS4 port for Windows

**NBA LIVE 19** (PlayStation 4, `CUSA11355`, update **1.01**) running as a **native x86-64**
Windows program, with **Live Run online** (3 vs 3 and 5 vs 5) on community servers. This is not
a CPU emulator: the game's original code runs directly on the PC's processor, a runtime replaces
the PS4 system libraries, and a renderer derived from [shadPS4](https://github.com/shadps4-emu/shadPS4)
translates the PS4's GPU work to Vulkan.

> **This is a fork of the UFC 3 native port by Gregori Sobrevía ([GNSB](https://github.com/GNSB)):
> [GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS](https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS).**
> The runtime, the renderer and the launcher are his work (see [`AUTHORS`](AUTHORS)); this fork
> adds what NBA LIVE 19 needs. That port is built on [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire),
> which comes from [bbport](https://github.com/deadinside28/bloodborne_pc). All code is
> GPL-2.0-or-later. The original UFC 3 README is [`README-UFC3.md`](README-UFC3.md).

> **No game files are included.** You need your own NBA LIVE 19 (`CUSA11355`) with its 1.01
> update. This project is not affiliated with or endorsed by Electronic Arts, the NBA or Sony
> Interactive Entertainment. *NBA LIVE*, *PlayStation* and related marks belong to their owners
> and are named only to identify the game.

---

## Download and play

1. Download `NBALive19Native-windows-x86_64.zip` from the [Releases](../../releases) page and
   extract it into a new, empty folder (not inside the game folder).
2. Run **`NBALive19.exe`**.
3. Choose the game:
   - **PKG...** and the game PKG: a "Game + Update" PKG with the 1.01 update built in, or the
     base game PKG with the 1.01 update PKG in the same folder (found by itself and merged in).
     They are unpacked once next to the PKGs (a progress bar shows it). Fake PKGs made from a dump only; store PKGs cannot be unpacked.
   - or **Folder...** and an extracted `CUSA11355` folder with the 1.01 update merged in.
4. Type your **Player name**: it is your name online (up to 16 characters). The first PC that
   uses a name keeps it; the launcher tells you when a name is already taken.
5. In **Server** (ONLINE), type the Live Run server's IPv4 address (its owner shares it).
6. Press **PLAY**. The first start prepares the game from your dump and builds the shader cache,
   so the first menus are slow; the next starts are fast.

**Requirements:** 64-bit Windows 10 (1903+) or 11, a GPU with a Vulkan 1.3 driver (6 GB of VRAM
or more recommended) and a CPU with AVX. Details, settings and troubleshooting:
[`packaging/windows/README-NBA-Windows.md`](packaging/windows/README-NBA-Windows.md) (also the
`README.md` inside the release).

## Online (Live Run)

- The game is always online and always in English. It connects to the server in the launcher's
  **Server** field: the IPv4 address of a Live Run server, which its owner shares (none is built
  in). In the game: **LIVE RUN → 3 vs 3 or
  5 vs 5 → search**: you join an open lobby or start a new one, and several lobbies run at the
  same time.
- The launcher's **ONLINE** panel lists the server's lobbies and the players online.
- Anyone can run a server: the server code is at
  [danilenko22/nba19-online-servidor](https://github.com/danilenko22/nba19-online-servidor). Put
  its IPv4 address in the **Server** field (it needs TCP 8088 open for the launcher's lobby list,
  besides the game's own ports).

## What this fork adds to the UFC 3 port

- **NBA LIVE 19 launch:** `NBALive19.exe` (the same launcher built with `-DNBA_LIVE_19`): game
  preparation from the dump, English always, 1080p, FPS limit 120 by default.
- **Online:** EA's online flow answered by a community server; Live Run's game server reached
  through the port; the PS4 patches the game needs online (no live-update failure logout, Live
  Run starting with 2 players, no idle disconnect, machine id from the network layer).
- **Launcher:** server field and lobby list, player name required and checked against the
  server, and installing from PKG files with
  [ShadPs4Plus PKG Extractor](https://github.com/AzaharPlus/shadPS4Plus/tree/PKG_EXTRACTOR_1_1)
  (GPL-2.0, in [`third_party/pkg_extractor`](third_party/pkg_extractor)).
- **Renderer and runtime:** fixes and performance work for this game (frame pacing at 120 FPS,
  readbacks for the player card textures, and more); see the commit history.

## Building

As the UFC 3 port: `scripts/build-windows.sh` (cross-built from Linux with llvm-mingw), then
`scripts/package-nba-windows.sh` makes the release archive. See [`README-UFC3.md`](README-UFC3.md)
for the toolchain.

## Licence

GPL-2.0-or-later ([`LICENSE`](LICENSE)); the upstream projects' notices are in [`AUTHORS`](AUTHORS)
and the bundled components' licences in [`packaging/windows/THIRD-PARTY.md`](packaging/windows/THIRD-PARTY.md).
