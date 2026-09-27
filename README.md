# Blueprint Engine

A from-scratch **Source-style game runtime**. Same idea as [Xash3D](https://github.com/FWGS/xash3d-fwgs) for GoldSrc: you keep the official game files, and this program is a separate engine that tries to load them.

Xash3D reads Half-Life 1 maps, models, and WADs without `hl.exe`. Blueprint reads **Source** content — BSP maps, VPK archives, VTF textures, Studio (`.mdl`) models, and `gameinfo.txt` — without `hl2.exe`. It is **not** a Source port, a mod for the official engine, or a finished game. It is an OpenGL experiment that can boot a mounted Half-Life 2 install and walk around inside it.

C++20, OpenGL 4.5 Core, GLFW, GLAD, GLM, Jolt Physics, miniaudio. No Unity, Unreal, or Godot.

```
cmake -B build
cmake --build build --config Release
```

---

## What it actually does

| Area | Status |
| --- | --- |
| `gameinfo.txt` search paths, loose folders, `*_dir.vpk` | Works for a normal Steam layout |
| BSP world, vis, lightmaps, light styles, skybox, 3D sky camera, fog | Good enough to look at a map |
| Brush collision through Jolt | Playable, not exact |
| Player walk, jump, duck, swim, noclip, sprint | Playable |
| Doors, buttons, `trigger_changelevel` + landmarks | Partial |
| Studio NPCs: citizen, Combine soldier, metrocop | Partial AI, speech, and animation |
| Weapons: crowbar, pistol, SMG, shotgun, gravity gun | Partial |
| Physics props and a few pickups | Partial |
| Main menu, chapters, loading screen, pause menu | Present, Valve-styled, not the real VGUI |
| Save games, multiplayer, particles, vehicles, most entity classes | Missing |

The window title becomes the `game` string from `gameinfo.txt` when a folder mounts.

---

## How it was built

This repo is **vibecoded**.

There was no engine design document and no plan to clone every Source system. Features showed up the way a late-night prototype does: load a BSP, then textures, then collision, then “the NPC should walk,” then “that door should open,” then “the pause menu should look orange.” Most of the code was written in [Cursor](https://cursor.com) with an AI pair, iterated until a map was fun to stand in, then committed. `.github/copilot-instructions.md` is the house rule: C++20, real OpenGL, no giant third-party engines.

That shows in the tree. `main.cpp` is still the loop, the demo room, the menu, and the map loader. Gameplay lives in `GameSimulation`. Navigation is an `info_node` graph (`AiNavigation`), not a baked nav mesh. Speech is a small response-rule reader (`NpcSpeech`), not the full Source response system. The UI is drawn with a custom backend that *looks* like old VGUI. It is not VGUI.

Expect sharp edges. If something works, it was poked until a specific map behaved. If something does not, nobody has written that system yet.

---

## Xash3D, but for Source

```
GoldSrc content  ->  Xash3D     instead of  hl.exe
Source content   ->  Blueprint  instead of  hl2.exe
```

You do **not** replace Valve’s engine and you do **not** copy game assets into this repository. Point Blueprint at a Source game directory you already own. It reads `gameinfo.txt`, expands `|gameinfo_path|` and `|all_source_engine_paths|`, mounts each search path, and opens `*_dir.vpk` archives beside loose files. A `custom/` folder is mounted first, same idea as Source.

Default game name is `hl2`. Relative names resolve next to the executable:

```
blueprinteng.exe
game/
  hl2/
    gameinfo.txt
    maps/
    materials/
    ...
```

An absolute path skips that layout:

```
blueprinteng.exe -game "C:\Program Files (x86)\Steam\steamapps\common\Half-Life 2\hl2"
```

`|all_source_engine_paths|` becomes the parent of that folder, so Episode One / Episode Two next to `hl2` can resolve the way they do in a normal Steam install. Other games are the same flag with another folder (`episodic`, `ep2`, `hl2mp`, a mod with its own `gameinfo.txt`).

This repository does not ship maps, textures, models, or sounds. Those stay in your Steam library.

---

## Build

You need:

- **CMake** 3.20 or newer
- A **C++20** compiler (Visual Studio 2022, or a recent GCC / Clang)
- **Git** on `PATH` (CMake FetchContent clones GLFW, GLAD, GLM, Jolt, stb, and miniaudio)
- A GPU and driver with **OpenGL 4.5**

Windows, Visual Studio generator:

```bat
cmake -B build
cmake --build build --config Release
```

The executable lands in `build\Release\blueprinteng.exe` (single-config generators put it in `build\blueprinteng` or `build\blueprinteng.exe`).

Ninja or Make:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The first configure is slow. It downloads the dependencies. Later builds are normal C++ compiles.

Shaders are **not** copied next to the exe. Paths are baked in at configure time from the source tree (`BLUEPRINT_SOURCE_DIR`). Move the repo and you need to reconfigure. Running the binary from another machine without those `shaders/` files will fail at startup.

---

## Run it

1. Install a Source game you own (Half-Life 2 is the one this has been aimed at).
2. Build Blueprint.
3. Launch with `-game` pointed at the folder that contains `gameinfo.txt`, not the Steam `common\Half-Life 2` root unless that root is the game folder. For retail Half-Life 2 the game folder is the `hl2` directory inside the install.

```bat
build\Release\blueprinteng.exe -game "D:\Steam\steamapps\common\Half-Life 2\hl2"
```

Or drop that game folder at `<exe>\game\hl2` and run with no arguments.

The console should print `Mounted N search paths` and one line per VPK. `Game directory not found` means the path is wrong.

### Menu

- **New Game** opens the chapter list from the mounted game’s chapter data.
- **Load Game** prints `save system not implemented yet`.
- Map files listed from `maps/*.bsp` can be picked when chapters are empty. Files treated as menu backgrounds stay out of that list.

### Command line

| Argument | Meaning |
| --- | --- |
| `-game <name-or-path>` | Game folder. Default `hl2`, resolved as `<exe>/game/hl2` when it is not absolute. `--game` works too. |
| `+map <path>` | BSP to load. A bare path that does not start with `-` or `+` is treated as a map too. |
| `--autostart` | Skip sitting in the menu and start the selected map. |

Examples:

```bat
blueprinteng.exe -game hl2 +map maps/d1_trainstation_01.bsp --autostart
blueprinteng.exe "C:\maps\my_test.bsp"
```

A direct `.bsp` path is for loose maps. Materials still come from the mounted game. A map with no mount falls back to `maps/` inside the source tree, then to the built-in gray room if nothing is there.

### In a map

| Key | Action |
| --- | --- |
| Mouse | Look (after the cursor is captured) |
| W A S D | Move |
| Space | Jump, or swim up |
| Left Ctrl | Duck, or swim down |
| Left Shift | Sprint |
| Left mouse | Fire |
| Right mouse | Alt fire |
| F | Toggle flashlight |
| V | Noclip |
| F11 | Fullscreen |
| F12 | Capture or release the mouse |
| Esc | Pause |
| Arrow keys | Look without the mouse |
| F2 | Collision wireframe |
| F3 | Debug overlay |
| F6 | Entity spawn labels |
| G | Drop a debug point light at the player |
| H | Throw a debug physics crate |

Level changes fire when you walk into a `trigger_changelevel`. The next map is loaded from the mount, and the player is lined up on the shared `info_landmark`. Props, weapons, and the three NPC types above can come along. Everyone else is left behind.

---

## Layout

```
src/main.cpp              window, menu flow, renderer, launch flags
src/bsp_loader.cpp        Source BSP
src/VpkArchive.cpp        VPK
src/vtf_loader.cpp        VTF
src/GameFileSystem.cpp    gameinfo.txt and search paths
src/StudioModel.cpp       .mdl draw and animation
src/PhysicsWorld.cpp      Jolt
src/PlayerController.cpp  movement
src/GameSimulation.cpp    entities, combat, transitions
src/AiNavigation.cpp      info_node paths
src/NpcSpeech.cpp         response concepts
src/ShadowManager.cpp     shadow maps
src/AudioSystem.cpp       miniaudio
src/ui/                   menu, pause, loading, labels
shaders/                  GLSL 450
```

---

## Bugs and missing pieces

Honest list. This is the current tree, not a roadmap promise.

**Loading**

- A bad or incomplete `gameinfo.txt` mounts the folder by itself and hopes.
- A VPK that fails to open is skipped with a log line. The map then loads with holes.
- Shader and UI font paths point at the source checkout. A copied exe without a rebuild does not find them.
- The process-RAM number on the debug overlay is implemented for Linux `/proc`. On Windows it stays at zero. VRAM uses NVIDIA or AMD extensions when the driver exposes them.

**Look**

- Only the nearest **24** map point lights and **8** spotlights are uploaded, plus the player flashlight, capped at **32** point lights total. Big maps drop distant lights.
- Shadows, the 3D sky, and fog exist and still miss cases (bad scale, missing sky camera, materials that need shaders Source would compile from `.vmt` proxies).
- Decals, detail textures, phong, rimlight, and the rest of the material system are incomplete. A lot of surfaces are “albedo plus lightmap plus a simple light.”

**Play**

- Save and load are stubs.
- NPC support is three classnames: `npc_citizen`, `npc_combine_s`, `npc_metropolice`. Other NPCs spawn as leftovers or not at all.
- AI is a short schedule (idle, alert, chase, shoot, cover, scripted sequence) on `info_node` links. No real nav mesh, no squads, no AI schedules from the game’s scripts.
- Speech is concept-to-WAV for those NPCs. Many sentences never play. Timing is rough.
- Combat, ammo, and the gravity gun are approximations. Hitscan and prop pickup will disagree with Half-Life 2.
- Doors and buttons cover `func_door`, `func_door_rotating`, `func_movelinear`, and `func_button`. Other brush entities are static or ignored.
- `trigger_changelevel` needs a landmark both maps share. A missing landmark logs an error and the transition feels wrong.
- Water is a contents check (water and slime), not a full water shader.
- The net graph is a local timing display. There is no network session.
- Physics uses simplified bodies. You will clip, bounce wrong, and find stuck spots. F2 draws the collision mesh when you want to see why.
- The pause menu and chapter UI are lookalikes. Options do not match a real Source options dialog.

**Not started**

Vehicles, helicopters, barns, vortigaunts, antlions, scanners, particle systems, facial flex, lip sync, HDR and bloom as Source does them, VScript, server plugins, demo playback, and multiplayer.

If a chapter loads and the next one dumps you in the void, that is this list, not your install.

---

## License of *this* code

Engine code in this repository is the project’s own work. **Valve’s maps, models, textures, sounds, and binaries are not included and are not licensed here.** Do not commit them. Buy the game, mount the folder, play.
