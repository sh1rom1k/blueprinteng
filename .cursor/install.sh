#!/usr/bin/env bash
# Cloud Agent install for Blueprint Engine (Linux).
# Installs the system libraries GLFW/Mesa need, then configures and builds the
# project. Safe to run repeatedly.
set -euo pipefail

cd "$(dirname "$0")/.."

export DEBIAN_FRONTEND=noninteractive

# The default toolchain resolves cc/c++ to clang, which cannot find libstdc++ on
# this image. GCC links cleanly, so the build is pinned to it below.
sudo apt-get update -qq
sudo apt-get install -y -qq --no-install-recommends \
  build-essential \
  cmake \
  git \
  python3 \
  python3-jinja2 \
  libstdc++-13-dev \
  libgl1-mesa-dev \
  libglu1-mesa-dev \
  mesa-common-dev \
  libgl1-mesa-dri \
  libx11-dev \
  libxrandr-dev \
  libxinerama-dev \
  libxcursor-dev \
  libxi-dev \
  libxext-dev \
  libwayland-dev \
  libxkbcommon-dev \
  wayland-protocols \
  extra-cmake-modules \
  xvfb \
  x11-apps \
  x11-utils \
  mesa-utils \
  ffmpeg \
  xauth

# GLFW, GLAD, GLM, Jolt, stb and miniaudio are fetched from GitHub by CMake.
CC=gcc CXX=g++ cmake -S . -B build
cmake --build build --parallel

echo "Blueprint Engine build complete: build/blueprinteng"
