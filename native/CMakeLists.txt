cmake_minimum_required(VERSION 3.20)
project(sessionscope_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REAPER extension: times REAPER's own processing of every audio block (decision 0003).
# REAPER loads extensions named reaper_*.dll from <resource path>/UserPlugins.
add_library(reaper_sessionscope SHARED ../hosts/reaper/extension/sessionscope_ext.cpp)
target_include_directories(reaper_sessionscope PRIVATE ../hosts/reaper/third_party/reaper-sdk)
set_target_properties(reaper_sessionscope PROPERTIES PREFIX "" OUTPUT_NAME "reaper_sessionscope")
if(MSVC)
  target_compile_options(reaper_sessionscope PRIVATE /W4 /permissive-)
  set_property(TARGET reaper_sessionscope PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")   # no runtime DLL to ship
else()
  target_compile_options(reaper_sessionscope PRIVATE -Wall -Wextra)
endif()

# CLAP sensor: a DLL named SessionScope.clap. Needs the CLAP headers: python tools/get_clap.py.
add_library(sessionscope_clap SHARED clap_sensor/sessionscope_clap.cpp)
target_include_directories(sessionscope_clap SYSTEM PRIVATE ../.toolchain/clap/include)
set_target_properties(sessionscope_clap PROPERTIES PREFIX "" OUTPUT_NAME "SessionScope" SUFFIX ".clap")
if(MSVC)
  target_compile_options(sessionscope_clap PRIVATE /W4 /permissive-)
  set_property(TARGET sessionscope_clap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_compile_options(sessionscope_clap PRIVATE -Wall -Wextra)
endif()
