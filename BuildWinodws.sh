#!/usr/bin/env bash
set -euo pipefail

echo "BuildWinodws.sh — cross-compile Windows (x86_64) from macOS"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

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

# Windows OpenCV (MinGW build) and ONNX Runtime (prebuilt DLL + import lib)
DEFAULT_WIN_OPENCV_PREFIX="/Users/rarespecies/Documents/folder/MedImgAIAnalyzer-cppServer/win_deps/install/opencv"
DEFAULT_WIN_ONNX_PREFIX="/Users/rarespecies/Documents/folder/win/onnxruntime-win-x64-1.24.1"
WIN_OPENCV_PREFIX="${WIN_OPENCV_PREFIX:-$DEFAULT_WIN_OPENCV_PREFIX}"
WIN_ONNX_PREFIX="${WIN_ONNX_PREFIX:-$DEFAULT_WIN_ONNX_PREFIX}"
if [ -z "$WIN_OPENCV_PREFIX" ]; then
  echo "ERROR: Set WIN_OPENCV_PREFIX to the Windows OpenCV root (contains include/ and x64/mingw/lib)." >&2
  exit 1
fi
if [ -z "$WIN_ONNX_PREFIX" ]; then
  echo "ERROR: Set WIN_ONNX_PREFIX to the Windows ONNX Runtime root (contains include/ and lib/)." >&2
  exit 1
fi

OPENCV_INC="-I${WIN_OPENCV_PREFIX}/include/opencv4"
OPENCV_LIB_DIR="${WIN_OPENCV_PREFIX}/lib"
if [ ! -d "$OPENCV_LIB_DIR" ]; then
  echo "ERROR: OpenCV MinGW libs not found at $OPENCV_LIB_DIR." >&2
  exit 1
fi
OPENCV_LIB_NAMES=(core imgcodecs imgproc highgui)
OPENCV_LIB="-L${OPENCV_LIB_DIR}"
for name in "${OPENCV_LIB_NAMES[@]}"; do
  match=("${OPENCV_LIB_DIR}/libopencv_${name}"*.dll.a)
  if [ ! -f "${match[0]}" ]; then
    echo "ERROR: OpenCV library libopencv_${name}*.dll.a not found in ${OPENCV_LIB_DIR}." >&2
    exit 1
  fi
  libfile="$(basename "${match[0]}")"
  libname="${libfile#lib}"
  libname="${libname%.dll.a}"
  OPENCV_LIB+=" -l${libname}"
done

ONNX_INC="-I${WIN_ONNX_PREFIX}/include"
ONNX_LIB_DIR="${WIN_ONNX_PREFIX}/lib"
ONNX_DLL="${ONNX_LIB_DIR}/onnxruntime.dll"
ONNX_IMPORT_LIB="${ONNX_LIB_DIR}/libonnxruntime.dll.a"
if [ ! -f "$ONNX_IMPORT_LIB" ]; then
  if [ ! -f "$ONNX_DLL" ]; then
    echo "ERROR: onnxruntime.dll not found at ${ONNX_DLL}." >&2
    exit 1
  fi
  if ! command -v gendef >/dev/null 2>&1; then
    echo "ERROR: gendef not found. Ensure llvm-mingw bin is on PATH." >&2
    exit 1
  fi
  if ! command -v "${TARGET_TRIPLE}-dlltool" >/dev/null 2>&1; then
    echo "ERROR: ${TARGET_TRIPLE}-dlltool not found. Ensure llvm-mingw bin is on PATH." >&2
    exit 1
  fi
  echo "Generating MinGW import library for onnxruntime.dll..."
  DEF_FILE="${ONNX_LIB_DIR}/onnxruntime.def"
  gendef "$ONNX_DLL" > "$DEF_FILE" || true
  if [ ! -s "$DEF_FILE" ]; then
    echo "gendef failed to extract exports; falling back to objdump." >&2
    if ! command -v "${TARGET_TRIPLE}-objdump" >/dev/null 2>&1; then
      echo "ERROR: ${TARGET_TRIPLE}-objdump not found. Ensure llvm-mingw bin is on PATH." >&2
      exit 1
    fi
    "${TARGET_TRIPLE}-objdump" -p "$ONNX_DLL" | awk '
      BEGIN { in_exports=0; print "LIBRARY onnxruntime.dll"; print "EXPORTS" }
      /^Export Table:/ { in_exports=1; next }
      /^Import Table:/ { exit }
      in_exports && $1 ~ /^[0-9]+$/ { print "  " $3 }
    ' > "$DEF_FILE"
  fi
  "${TARGET_TRIPLE}-dlltool" -d "$DEF_FILE" -l "$ONNX_IMPORT_LIB" -D "onnxruntime.dll"
fi
ONNX_LIB="-L${ONNX_LIB_DIR} -lonnxruntime"

SYSROOT_INC="${LLVM_MINGW_ROOT}/${TARGET_TRIPLE}/include"
SYSROOT_LIB="${LLVM_MINGW_ROOT}/${TARGET_TRIPLE}/lib"
ZLIB_SRC_DIR="${SCRIPT_DIR}/win_deps/src/opencv/3rdparty/zlib"
ZLIB_BUILD_DIR="${SCRIPT_DIR}/win_deps/build/zlib"
ZLIB_LIB="${ZLIB_BUILD_DIR}/libz.a"
if [ ! -f "${SYSROOT_INC}/zlib.h" ] && [ ! -f "${ZLIB_SRC_DIR}/zlib.h" ]; then
  echo "ERROR: zlib.h not found in sysroot or ${ZLIB_SRC_DIR}." >&2
  exit 1
fi
if [ ! -f "${ZLIB_SRC_DIR}/zconf.h" ] && [ -f "${ZLIB_SRC_DIR}/zconf.h.cmakein" ]; then
  echo "Generating zconf.h from zconf.h.cmakein..."
    sed -e 's/#cmakedefine Z_PREFIX/\/\* #undef Z_PREFIX \*\//' \
      -e 's/#cmakedefine Z_HAVE_UNISTD_H/\/\* #undef Z_HAVE_UNISTD_H \*\//' \
      "${ZLIB_SRC_DIR}/zconf.h.cmakein" > "${ZLIB_SRC_DIR}/zconf.h"
fi
if [ ! -f "${ZLIB_LIB}" ]; then
  echo "Building zlib (Windows static) from ${ZLIB_SRC_DIR}..."
  mkdir -p "${ZLIB_BUILD_DIR}"
  C_SRC=("adler32.c" "compress.c" "crc32.c" "deflate.c" "gzclose.c" "gzlib.c" "gzread.c" "gzwrite.c" "infback.c" "inffast.c" "inflate.c" "inftrees.c" "trees.c" "uncompr.c" "zutil.c")
  OBJ_FILES=()
  for src in "${C_SRC[@]}"; do
    "${TARGET_TRIPLE}-clang" -O2 -I"${ZLIB_SRC_DIR}" -c "${ZLIB_SRC_DIR}/${src}" -o "${ZLIB_BUILD_DIR}/${src%.c}.o"
    OBJ_FILES+=("${ZLIB_BUILD_DIR}/${src%.c}.o")
  done
  "${TARGET_TRIPLE}-ar" rcs "${ZLIB_LIB}" "${OBJ_FILES[@]}"
fi

# Put ONNX/OpenCV includes before Homebrew includes to avoid host headers.
CXXFLAGS="-std=c++17 -O2 -I./include -I./cnpy ${ONNX_INC} ${OPENCV_INC} -I${ZLIB_SRC_DIR} -I${SYSROOT_INC} -I${CROW_INCLUDE}"
LDFLAGS="${OPENCV_LIB} ${ONNX_LIB} -L${SYSROOT_LIB} -L${ZLIB_BUILD_DIR} -lz -lws2_32 -lmswsock -lwinmm -lole32"

OUT="main.exe"

echo "$CXX $CXXFLAGS main.cpp cnpy/cnpy.cpp -o $OUT $LDFLAGS"
"$CXX" $CXXFLAGS main.cpp cnpy/cnpy.cpp -o "$OUT" $LDFLAGS

echo "Build complete: ./$OUT"