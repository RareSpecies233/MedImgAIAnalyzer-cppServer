#!/usr/bin/env bash
set -euo pipefail

echo "BuildmacOS.sh — 使用 Homebrew 的 Crow 编译示例"

BREW_PREFIX=""
if command -v brew >/dev/null 2>&1; then
  BREW_PREFIX="$(brew --prefix)"
  echo "Homebrew prefix detected: $BREW_PREFIX"
fi

# 选择 include 路径
if [ -n "${BREW_PREFIX}" ] && [ -f "${BREW_PREFIX}/include/crow/app.h" ]; then
  EXTRA_INCLUDES="-I${BREW_PREFIX}/include"
  echo "Using Crow headers from: ${BREW_PREFIX}/include/crow"
else
  EXTRA_INCLUDES="-I./include"
  echo "Using local include: ./include (no Homebrew Crow found)"
fi

# OpenCV flags
OPENCV_CFLAGS=""
OPENCV_LIBS=""
if command -v pkg-config >/dev/null 2>&1; then
  if pkg-config --exists opencv4; then
    OPENCV_CFLAGS="$(pkg-config --cflags opencv4)"
    OPENCV_LIBS="$(pkg-config --libs opencv4)"
  elif pkg-config --exists opencv; then
    OPENCV_CFLAGS="$(pkg-config --cflags opencv)"
    OPENCV_LIBS="$(pkg-config --libs opencv)"
  fi
fi

# ONNX Runtime flags
ONNX_CFLAGS=""
ONNX_LIBS=""
if command -v pkg-config >/dev/null 2>&1; then
  if pkg-config --exists onnxruntime; then
    ONNX_CFLAGS="$(pkg-config --cflags onnxruntime)"
    ONNX_LIBS="$(pkg-config --libs onnxruntime)"
  fi
fi

if [ -z "${ONNX_CFLAGS}" ] && [ -n "${BREW_PREFIX}" ]; then
  if brew --prefix onnxruntime >/dev/null 2>&1; then
    ONNX_PREFIX="$(brew --prefix onnxruntime)"
    ONNX_CFLAGS="-I${ONNX_PREFIX}/include"
    ONNX_LIBS="-L${ONNX_PREFIX}/lib -lonnxruntime"
  fi
fi

if [ -z "${ONNX_CFLAGS}" ]; then
  echo "ERROR: ONNX Runtime headers not found. Please install onnxruntime (brew install onnxruntime) or set pkg-config." >&2
  exit 1
fi

CXX=clang++
CXXFLAGS="-std=c++17 -stdlib=libc++ -g ${EXTRA_INCLUDES} -I./include -I./cnpy ${OPENCV_CFLAGS} ${ONNX_CFLAGS}"

echo "$CXX $CXXFLAGS main.cpp cnpy/cnpy.cpp -o main ${OPENCV_LIBS} ${ONNX_LIBS} -lz"
$CXX $CXXFLAGS main.cpp cnpy/cnpy.cpp -o main ${OPENCV_LIBS} ${ONNX_LIBS} -lz

echo "哈基米南北绿豆!!!!!!!!"
echo "Build complete: ./main"
echo "Run: ./main"
