cmake_minimum_required(VERSION 3.20)
project(MedImgAIAnalyzer LANGUAGES CXX)

# User-configurable options
option(USE_VCPKG "Use vcpkg (will be used if available)" ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Prefer Release by default for developer builds on Windows
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# Fetch Crow (header-only) so project can build without system-wide crow
FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/CrowCpp/crow.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(crow)

# Project sources
file(GLOB PROJECT_SOURCES
  "${CMAKE_SOURCE_DIR}/*.cpp"
  "${CMAKE_SOURCE_DIR}/cnpy/*.cpp"
)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/cnpy
  ${crow_SOURCE_DIR}/include
)

# Crow requires standalone Asio headers
find_package(asio CONFIG QUIET)
if(asio_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE asio::asio)
else()
  find_path(ASIO_INCLUDE_DIR asio.hpp
    PATHS
      /opt/homebrew/include
      /usr/local/include
  )
  if(ASIO_INCLUDE_DIR)
    message(STATUS "Found Asio headers: ${ASIO_INCLUDE_DIR}")
    target_include_directories(${PROJECT_NAME} PRIVATE ${ASIO_INCLUDE_DIR})
  else()
    message(FATAL_ERROR "Asio not found. Please install Asio headers (brew install asio) or provide asioConfig.cmake.")
  endif()
endif()

# Compiler warnings and sane defaults
# Source charset: UTF-8 (source files).
# On Windows with Clang/LLVM, execution charset is already UTF-8 by default;
# we handle console encoding at runtime via SetConsoleOutputCP(CP_UTF8).
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE
    /W4 /permissive-
    /source-charset:utf-8
    /execution-charset:utf-8
  )
else()
  target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
    -finput-charset=utf-8
  )
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
  endif()
endif()

# Find OpenCV (required)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
if(NOT OpenCV_FOUND)
  message(FATAL_ERROR "OpenCV not found. Install OpenCV or use vcpkg: `vcpkg install opencv`.")
endif()
message(STATUS "Found OpenCV: ${OpenCV_VERSION}")
target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})

# cnpy requires zlib
find_package(ZLIB REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)

# Find ONNX Runtime (optional but required for inference features)
find_package(ONNXRuntime CONFIG QUIET)
if(ONNXRuntime_FOUND)
  message(STATUS "Found ONNX Runtime (CONFIG): ${ONNXRuntime_VERSION}")
  target_link_libraries(${PROJECT_NAME} PRIVATE onnxruntime::onnxruntime)
else()
  find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h)
  find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime libonnxruntime)
  if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    message(STATUS "Found ONNX Runtime (manual)")
    target_include_directories(${PROJECT_NAME} PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ONNXRUNTIME_LIBRARY})
  else()
    message(WARNING "ONNX Runtime not found. Inference-related APIs will fail to link if used. To enable, install onnxruntime (vcpkg: onnxruntime) or set ONNXRUNTIME_DIR.")
  endif()
endif()

# Windows-specific link libs often needed for networking
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi)
endif()

# Where to place the produced exe / DLLs
set_target_properties(${PROJECT_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
)

# Helpful cache variables for users
set(CMAKE_INSTALL_BINDIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Executable output directory")

# Provide a small summary
message(STATUS "Project configured for: ${CMAKE_SYSTEM}")
