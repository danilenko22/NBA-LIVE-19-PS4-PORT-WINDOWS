# NBA LIVE 19 native port — Windows

This archive runs **NBA LIVE 19** (PlayStation 4, `CUSA11355`) as a native Windows x86-64
program, on the runtime of the UFC 3 native port (SacredFire). It **contains no game files**:
you need your own dump of the game.

## Requirements

- 64-bit Windows 10 (1903 or newer) or Windows 11.
- A GPU with a Vulkan 1.3 driver. 6 GB of VRAM or more is recommended.
- A CPU with AVX (Intel Haswell / 4th generation or newer, or AMD Ryzen).
- The game, either way:
  - its **PKG files**: a game PKG with the 1.01 update built in ("Game + Update"), or the base
    game PKG and the 1.01 update PKG in the same folder (fake PKGs made from a dump; store PKGs
    cannot be unpacked), or
  - the extracted `CUSA11355` folder with the 1.01 update merged in (`eboot.bin`,
    `data\kettle\EAWebkit.prx` and the `.big` files inside it).

## Playing

1. Extract the archive anywhere (not inside the game folder).
2. Run **`NBALive19.exe`**.
3. Choose the game: **PKG...** and the game PKG (with the update built in, or the update
   PKG next to it, found and merged by itself; this unpacks them once, next to the PKGs, and takes several minutes and as
   much free disk space as the PKGs), or **Folder...** for an extracted `CUSA11355` folder.
4. Type your **Player name**: it is your name online (up to 16 characters, letters and numbers).
   The first PC to use a name keeps it; the launcher says so when a name is already taken.
5. In **Server** (ONLINE), type the Live Run server's IPv4 address (its owner shares it) and
   press **Connect** to see its lobbies.
6. Press **PLAY**.

The first start prepares the game from your dump (about a minute) and builds the shader cache,
so the menus are slow the first time only. Data, saves, the shader cache and logs live in
`%LOCALAPPDATA%\NBALive19Native`.

The game is always in English and always online: it connects to the Live Run server in the
launcher's **Server** field (ONLINE). Type the server's IPv4 address there: the person who runs
the server shares it. No server is built in.

### Launcher settings

- **Controller** — the gamepad for player 1; **Controls...** maps keyboard keys and gamepad
  buttons. The keyboard and a gamepad work together.
- **Server** and the lobby list (ONLINE) — the Live Run server (ours by default; anyone can run
  one) and its lobbies and online players. In the game: LIVE RUN, 3 vs 3 or 5 vs 5, search.
- **Game resolution** — 1080p (the game draws at 1080p).
- **Display resolution** — the window size; on a large screen use **Upscaling = AMD FSR 1.0**
  and some **Sharpening**. In fullscreen the monitor's size is used.
- **Present mode** — *Immediate* (uncapped, may tear), *Mailbox* or *VSync*.
- **FPS limit** — 60 / 90 / 120 / 144 / 240 / Unlimited.
- In game: **Insert** or **L3+R3** opens the overlay menu.

## Logs and problems

The launcher does not show the game's log: each run writes it to
`%LOCALAPPDATA%\NBALive19Native\logs` (**Open logs** in the launcher). Send the newest log when
you report a problem.

- **The game closes at start after an update of the port or of the graphics driver** — start it
  again: the port rebuilds its shader cache by itself when it was made by another version or
  when the previous start stopped while loading it.
- **Very slow menus** — the first start compiles the shaders; the next starts are fast.
- **Clear the shader cache at start** (Advanced) builds it again from scratch.

## Source and licences

Built from the UFC 3 native port's source with the NBA LIVE 19 changes (see `VERSION`). The
licences of the components are in `licenses\`. GPL-2.0-or-later (see `LICENSE`).
