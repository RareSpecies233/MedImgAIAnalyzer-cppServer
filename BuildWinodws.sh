#!/usr/bin/env bash
set -euo pipefail

echo "BuildWinodws.sh — cross-compile Windows (x86_64) from macOS"

TARGET_TRIPLE="x86_64-w64-mingw32"
if [ "${WIN_ARCH:-x86_64}" = "i686" ]; then
  TARGET_TRIPLE="i686-w64-mingw32"
fi

DEFAULT_LLVM_MINGW_ROOT="/Users/rarespecies/Documents/folder/llvm-mingw-20251216-ucrt-macos-universal"
LLVM_MINGW_ROOT="${LLVM_MINGW_ROOT:-$DEFAULT_LLVM_MINGW_ROOT}"
if [ -d "$LLVM_MINGW_ROOT/bin" ]; then
  export PATH="$LLVM_MINGW_ROOT/bin:$PATH"
fi

CXX="${TARGET_TRIPLE}-clang++"
if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "ERROR: $CXX not found. Set LLVM_MINGW_ROOT to your llvm-mingw path." >&2
  exit 1
fi

BREW_PREFIX=""
if command -v brew >/dev/null 2>&1; then
  BREW_PREFIX="$(brew --prefix)"
fi

# Crow headers (header-only)
CROW_INCLUDE="${CROW_INCLUDE:-}"
if [ -z "$CROW_INCLUDE" ] && [ -n "$BREW_PREFIX" ] && [ -f "${BREW_PREFIX}/include/crow/app.h" ]; then
  CROW_INCLUDE="${BREW_PREFIX}/include"
fi
if [ -z "$CROW_INCLUDE" ]; then
  echo "ERROR: Crow headers not found. Set CROW_INCLUDE to your Crow include path." >&2
  exit 1
fi

# Windows OpenCV and ONNX Runtime (must be Windows builds)
DEFAULT_WIN_DEPS="/Users/rarespecies/Documents/folder/win"
WIN_OPENCV_PREFIX="${WIN_OPENCV_PREFIX:-$DEFAULT_WIN_DEPS/opencv/build}"
WIN_ONNX_PREFIX="${WIN_ONNX_PREFIX:-$DEFAULT_WIN_DEPS/onnxruntime-win-x64-1.24.1}"
if [ -z "$WIN_OPENCV_PREFIX" ]; then
  echo "ERROR: Set WIN_OPENCV_PREFIX to the Windows OpenCV root (contains include/ and x64/mingw/lib)." >&2
  exit 1
fi
if [ -z "$WIN_ONNX_PREFIX" ]; then
  echo "ERROR: Set WIN_ONNX_PREFIX to the Windows ONNX Runtime root (contains include/ and lib/)." >&2
  exit 1
fi

OPENCV_INC="-I${WIN_OPENCV_PREFIX}/include"
OPENCV_LIB_DIR="${WIN_OPENCV_PREFIX}/x64/mingw/lib"
if [ ! -d "$OPENCV_LIB_DIR" ]; then
  echo "ERROR: OpenCV MinGW libs not found at $OPENCV_LIB_DIR." >&2
  echo "Hint: your OpenCV looks like an MSVC build (vc16). You need MinGW-built OpenCV libs." >&2
  exit 1
fi
OPENCV_LIB="-L${OPENCV_LIB_DIR} -lopencv_core -lopencv_imgcodecs -lopencv_imgproc -lopencv_highgui"

ONNX_INC="-I${WIN_ONNX_PREFIX}/include"
ONNX_LIB="-L${WIN_ONNX_PREFIX}/lib -lonnxruntime"

CXXFLAGS="-std=c++17 -O2 -I./include -I./cnpy -I${CROW_INCLUDE} ${OPENCV_INC} ${ONNX_INC}"
LDFLAGS="${OPENCV_LIB} ${ONNX_LIB} -lz -lws2_32 -lwinmm"

OUT="main.exe"

echo "$CXX $CXXFLAGS main.cpp cnpy/cnpy.cpp -o $OUT $LDFLAGS"
"$CXX" $CXXFLAGS main.cpp cnpy/cnpy.cpp -o "$OUT" $LDFLAGS

echo "Build complete: ./$OUT"