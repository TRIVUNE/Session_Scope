# =================================================================================================
# CMakeLists.txt - how SessionScope is built
# =================================================================================================
#
# CMake reads this file and works out how to compile everything; Ninja then does the compiling.
# From the repository folder (see README.md):
#
#   cmake --preset windows         # once (and after adding files): set up build/cmake
#   cmake --build --preset windows # compile what changed
#   ctest --preset windows         # run every test
#   (on a Mac: the same with "mac")
#
# What gets built, and where:
#   build/engine/sessionscope.exe     the console program (record, report, simulate...)
#   build/engine/SessionScopeHUD.exe  the HUD window
#   build/engine/tests/test_*.exe     the tests
#
# The pieces are described in engine/CMakeLists.txt (engine, console program, HUD, tests) and
# cmake/third_party.cmake (GLFW, Dear ImGui, stb, Inter). Everything third-party is downloaded into
# .toolchain/ the first time, pinned and checked (cmake/fetch.cmake).

cmake_minimum_required(VERSION 3.25)
project(SessionScope LANGUAGES C CXX)

# The C++ version our code is written in.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)   # optimised (-O2-like); the presets set this too
endif()

# Where the programs land: build/engine, the same place the README, guide and handoff notes use.
set(SESSIONSCOPE_OUT "${CMAKE_SOURCE_DIR}/build/engine")

include(cmake/fetch.cmake)
include(cmake/third_party.cmake)

enable_testing()
add_subdirectory(engine)
