#!/bin/sh
set -eu

APP_NAME="appimage-nurse"
SRC="src/appimage_nurse.c"
BUILD_DIR="build"
OUT="$BUILD_DIR/$APP_NAME"

cc=${CC:-gcc}

if ! command -v "$cc" >/dev/null 2>&1; then
  echo "Error: C compiler not found: $cc"
  echo "Install gcc or clang, then try again."
  exit 1
fi

if [ ! -f "$SRC" ]; then
  echo "Error: source file not found: $SRC"
  echo "Run this from the repository root."
  exit 1
fi

mkdir -p "$BUILD_DIR"

"$cc" -std=c11 -O2 -Wall -Wextra -o "$OUT" "$SRC"
chmod +x "$OUT"

echo "Built: $OUT"
