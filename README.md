# Blueprint Engine

Minimal C++20 OpenGL 4.5 Core Profile application using GLFW, GLAD and GLM.

## Windows

Install Visual Studio 2022 with the Desktop development with C++ workload, CMake 3.20 or newer, Git, and Python 3 with pip. GLAD generates its OpenGL loader at build time and needs the Python package jinja2. Configure installs that package when it is missing. A GPU driver with OpenGL 4.5 is required to launch the window.

```bat
build-windows.bat
```

That script configures the newest installed Visual Studio generator and builds `build\Release\blueprinteng.exe`. The executable uses the static Visual C++ runtime, so it does not need a separate redistributable. It is marked per-monitor DPI aware and uses UTF-8 paths.

The build copies shaders, maps, sounds, textures, and fonts next to the executable. The program still prefers the source tree it was compiled from, so shader edits are picked up without copying them again. If that tree is not present, it loads the files beside the executable.



