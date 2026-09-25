# Blueprint OpenGL

Minimal C++20 OpenGL 4.5 Core Profile application using GLFW, GLAD and GLM.

## Build

```sh
python3 -m venv .venv
.venv/bin/python -m pip install jinja2
cmake -B build -DPython_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build
./build/blueprinteng                 # colored rotating cube
./build/blueprinteng /path/to/map.bsp # render Source BSP geometry
```

CMake downloads GLFW, GLAD and GLM with `FetchContent` during configuration. GLAD uses Jinja2 during code generation, so the commands above prepare it in a local virtual environment. The application opens a window, renders a colored cube, and rotates it using Model, View and Projection matrices.

`Shader` handles GLSL compilation and linking. `Mesh` owns the cube's VAO, VBO and EBO and exposes `Draw()`.

`BspLoader` reads the Source BSP `VERTEXES`, `EDGES`, `SURFEDGES` and `FACES` lumps, triangulates each face, normalizes the map bounds, and uploads the result through `Mesh`. It also builds a separate collision mesh for Jolt from non-trigger faces. Materials containing `trigger` are rendered but deliberately excluded from collision.

`PhysicsWorld` uses Jolt `MeshShape` for the static BSP world and `CharacterVirtual` for the player capsule. This gives solid BSP geometry slope/wall/floor collision; `V` bypasses the Jolt character movement for noclip.

`vtf::LoadVtfTexture()` in `include/VtfTexture.hpp` supports legacy VTF 7.1/7.2 textures in `RGBA8888`, `BGRA8888`, `DXT1`, `DXT3` and `DXT5` formats. The loader also reads VPK v1/v2 directory archives and extracts VTF data directly from them. It returns a `GLuint`; release it with `glDeleteTextures()` when no longer needed. VTF 7.3+ resource-directory layouts are rejected explicitly until their resource table is supported.

For BSP materials, place textures relative to the map file:

```text
maps/example.bsp
textures/brick/brickwall001a.vtf
```

The loader reads material names from `TEXINFO/TEXDATA` and first searches `<project>/textures/<material>.vtf`, then the map/game `textures` and `materials` directories. Missing or unsupported textures use a colored fallback.

If a VTF/VPK material is missing or unsupported, the loader uses `<project>/textures/placeholder.jpg` as the visual fallback.

VPK archives can be placed in `textures/`. Keep all parts together, for example `pak01_dir.vpk` with `pak01_000.vpk`, `pak01_001.vpk`, and so on. The loader searches the archive for `materials/<material>.vtf` automatically.

When a BSP map is loaded, use `WASD` to move, the mouse to look, `Space` to jump, `Ctrl` to descend in noclip, `V` to toggle noclip, `Shift` for faster movement, `F11` to toggle fullscreen, `F12` to release or capture the mouse, and `Esc` to pause.
