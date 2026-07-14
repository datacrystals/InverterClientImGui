#!/usr/bin/env bash
# Build script for imgui_api_viewer
# Usage:
#   ./build.sh           # configure + build (Release)
#   ./build.sh debug     # configure + build (Debug)
#   ./build.sh clean     # remove build directory
#   ./build.sh run       # build, then run the executable

set -euo pipefail

BUILD_DIR="build"
BUILD_TYPE="Release"

case "${1:-}" in
  clean)
    rm -rf "$BUILD_DIR"
    echo "Removed $BUILD_DIR"
    exit 0
    ;;
  debug)
    BUILD_TYPE="Debug"
    ;;
  run)
    RUN_AFTER=1
    ;;
esac

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --parallel

if [[ "${RUN_AFTER:-0}" == "1" ]]; then
  exec "./$BUILD_DIR/imgui_api_viewer"
fi
