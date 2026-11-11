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
# Or all of it in one command, ending with the plug-ins installed where DAWs look for them:
#
#   cmake --workflow --preset windows   # set up, compile, test, install
#
# What gets built, and where:
#   build/engine/sessionscope.exe     the console program (record, report, simulate...)
#   build/engine/SessionScopeHUD.exe  the HUD window
#   build/engine/tests/test_*.exe     the tests
#   build/native/                     the sensor plug-ins and the REAPER extension (native/CMakeLists.txt)
#
#   ctest --preset windows            the tests (fast, no plug-ins loaded)
#   ctest --preset windows-plugins    the official plug-in validators (close your DAW first)
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
add_subdirectory(native)

# ---- the REAPER add-on's Lua tests (hosts/reaper/tests): the Lua modules under a fake REAPER,
# run by Node. Only added when Node is installed and `npm install` has been run there once
# (it fetches fengari, a Lua written in JavaScript); re-run `cmake --preset ...` afterwards.
find_program(NODE_PROGRAM node)
set(lua_tests "${CMAKE_SOURCE_DIR}/hosts/reaper/tests")
if(NODE_PROGRAM AND EXISTS "${lua_tests}/node_modules/fengari")
  add_test(NAME reaper_lua COMMAND "${NODE_PROGRAM}" run.js WORKING_DIRECTORY "${lua_tests}")
endif()

# ---- installing: `cmake --build --preset windows-install`, the last step of the workflow preset
# Runs the console program's own install command (engine/src/install.h): it copies the plug-ins
# into the per-user and shared plug-in folders and the REAPER extension into REAPER.
add_custom_target(install_sessionscope
                  COMMAND sessionscope install --from "${CMAKE_SOURCE_DIR}/build/native"
                  USES_TERMINAL   # show its messages as they come
                  COMMENT "Installing the SessionScope plug-ins")
# Everything it copies must be built first (the names differ a little between Windows and macOS).
foreach(plugin vst3_sensor vst3_probe clap_sensor clap_probe au_sensor reaper_extension)
  if(TARGET ${plugin})
    add_dependencies(install_sessionscope ${plugin})
  endif()
endforeach()
