#!/usr/bin/env bash
# Cloud Agent start for Blueprint Engine (Linux).
# The engine opens a GLFW/OpenGL window, so a display server must exist. This VM
# has no GPU or physical display, so a virtual X server (Xvfb) is started and
# rendering falls back to Mesa's software rasterizer (llvmpipe), which provides
# the OpenGL 4.5 core profile the engine requires.
set -euo pipefail

DISPLAY_NUM=99

# Idempotent: only start Xvfb if this display is not already being served.
if ! xdpyinfo -display ":${DISPLAY_NUM}" >/dev/null 2>&1; then
  rm -f "/tmp/.X${DISPLAY_NUM}-lock"
  Xvfb ":${DISPLAY_NUM}" -screen 0 1280x720x24 -ac +extension GLX +render -noreset \
    >/tmp/xvfb.log 2>&1 &
  # Wait for the server to accept connections.
  for _ in $(seq 1 30); do
    if xdpyinfo -display ":${DISPLAY_NUM}" >/dev/null 2>&1; then
      break
    fi
    sleep 0.2
  done
fi

echo "Virtual display ready on :${DISPLAY_NUM}. Run the engine with:"
echo "  DISPLAY=:${DISPLAY_NUM} LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ./build/blueprinteng"
