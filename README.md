# Blueprint Engine

Minimal C++20 OpenGL 4.5 Core Profile application using GLFW, GLAD and GLM.

## Windows

Install Visual Studio 2022 with the Desktop development with C++ workload, CMake 3.20 or newer, Git, and Python 3 with pip. GLAD generates its OpenGL loader at build time and needs the Python package jinja2. Configure installs that package when it is missing. A GPU driver with OpenGL 4.5 is required to launch the window.

```bat
build-windows.bat
```

That script configures the newest installed Visual Studio generator and builds `build\Release\blueprinteng.exe`. The executable uses the static Visual C++ runtime, so it does not need a separate redistributable. It is marked per-monitor DPI aware and uses UTF-8 paths.

The build copies shaders, maps, sounds, textures, and fonts next to the executable. The program still prefers the source tree it was compiled from, so shader edits are picked up without copying them again. If that tree is not present, it loads the files beside the executable.

## Linux

Install a C++20 toolchain (GCC), CMake 3.20 or newer, Git, Python 3, and the GLFW build dependencies (Mesa GL, X11 and Wayland development headers). On Debian/Ubuntu this is captured by `.cursor/install.sh`, which installs the packages and then builds the project:

```sh
    bash .cursor/install.sh
```

That produces `build/blueprinteng`. The default `cc`/`c++` on some images resolve to Clang, which cannot find `libstdc++` here, so the build is pinned to GCC with `CC=gcc CXX=g++`.

The engine opens an OpenGL 4.5 core-profile window and needs a display. On a machine without a GPU or physical display, run it against a virtual X server with Mesa's software rasterizer (`llvmpipe`), which provides OpenGL 4.5. `.cursor/start.sh` starts such a display on `:99`:

```sh
    bash .cursor/start.sh
    DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ./build/blueprinteng
```

On a workstation with a GPU driver, just run `./build/blueprinteng` directly.



