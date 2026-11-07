# =================================================================================================
# zig-toolchain.cmake - the compiler for Windows builds: Zig's built-in clang
# =================================================================================================
#
# CMake reads this file before anything else (CMakePresets.json names it as the "toolchain file").
# It makes sure the pinned Zig compiler is in .toolchain/, downloading and checking it the first
# time, and tells CMake to compile C and C++ with it.
#
# Why Zig: it is one download (about 80 MB) that contains a complete clang compiler and the Windows
# headers and libraries, so building needs no Visual Studio. Nothing in SessionScope is written in
# the Zig language; only its compiler is used ("zig cc" for C, "zig c++" for C++).
#
# Where it comes from: Zig 0.16.0 from ziglang.org; the SHA-256 fingerprint below is the one
# published in https://ziglang.org/download/index.json (checked 27 Oct 2026). If the download
# doesn't match it, CMake stops instead of using it.

set(ZIG_VERSION 0.16.0)
set(ZIG_ARCHIVE "zig-x86_64-windows-${ZIG_VERSION}.zip")
set(ZIG_SHA256 68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e)
get_filename_component(TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../.toolchain" ABSOLUTE)
set(ZIG_DIR "${TOOLCHAIN_DIR}/zig-x86_64-windows-${ZIG_VERSION}")
set(ZIG "${ZIG_DIR}/zig.exe")

if(NOT EXISTS "${ZIG}")
  message(STATUS "Downloading Zig ${ZIG_VERSION} (the compiler, once)")
  file(DOWNLOAD "https://ziglang.org/download/${ZIG_VERSION}/${ZIG_ARCHIVE}" "${TOOLCHAIN_DIR}/${ZIG_ARCHIVE}"
       EXPECTED_HASH SHA256=${ZIG_SHA256} SHOW_PROGRESS)
  file(ARCHIVE_EXTRACT INPUT "${TOOLCHAIN_DIR}/${ZIG_ARCHIVE}" DESTINATION "${TOOLCHAIN_DIR}")
  file(REMOVE "${TOOLCHAIN_DIR}/${ZIG_ARCHIVE}")
endif()

# The compilers: zig.exe followed by "cc" or "c++".
set(CMAKE_C_COMPILER "${ZIG}")
set(CMAKE_C_COMPILER_ARG1 cc)
set(CMAKE_CXX_COMPILER "${ZIG}")
set(CMAKE_CXX_COMPILER_ARG1 c++)

# CMake also wants an "archiver" (for .a libraries) and "ranlib". Zig has both as "zig ar" and
# "zig ranlib"; CMake needs them as single programs, so two tiny .cmd files pass the word on.
file(WRITE "${TOOLCHAIN_DIR}/zig-ar.cmd" "@\"${ZIG}\" ar %*\n")
file(WRITE "${TOOLCHAIN_DIR}/zig-ranlib.cmd" "@\"${ZIG}\" ranlib %*\n")
set(CMAKE_AR "${TOOLCHAIN_DIR}/zig-ar.cmd")
set(CMAKE_RANLIB "${TOOLCHAIN_DIR}/zig-ranlib.cmd")
