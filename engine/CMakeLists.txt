# =================================================================================================
# engine/CMakeLists.txt - the engine, the console program, the HUD and the tests
# =================================================================================================
#
#   engine            every engine/src/*.cpp, shared by the three below (an "object library": the
#                     compiled files are linked into each program, no separate .lib)
#   sessionscope      the console program (engine/tools/sessionscope.cpp)
#   SessionScopeHUD   the HUD window (engine/hud/**/*.cpp, plus GLFW and Dear ImGui)
#   test_*            one program per engine/tests/test_*.cpp, each run by ctest

# Our own code: every warning on, and every warning is an error, so problems can't pile up.
set(OUR_WARNINGS -Wall -Wextra -Werror)

# ---- the engine
file(GLOB engine_files CONFIGURE_DEPENDS src/*.cpp)
add_library(engine OBJECT ${engine_files})
target_include_directories(engine PUBLIC src ../native/sensor_core)
target_compile_options(engine PRIVATE ${OUR_WARNINGS})

# ---- the console program
add_executable(sessionscope tools/sessionscope.cpp)
target_link_libraries(sessionscope PRIVATE engine)
target_compile_options(sessionscope PRIVATE ${OUR_WARNINGS})
set_target_properties(sessionscope PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SESSIONSCOPE_OUT}")

# ---- the HUD
# Its fonts become C++ source first (cmake/embed_fonts.cmake), rebuilt when a font file changes.
set(fonts_cpp "${CMAKE_CURRENT_BINARY_DIR}/fonts_data.cpp")
set(font_files "${INTER_DIR}/Inter-Regular.ttf" "${INTER_DIR}/InterDisplay-Light.ttf")
# The list of fonts goes in a small file (one "NAME=file" per line): a list on the command line
# would be cut apart by the system's shell.
set(fonts_list "${CMAKE_CURRENT_BINARY_DIR}/fonts.list")
file(WRITE "${fonts_list}" "INTER_REGULAR=${INTER_DIR}/Inter-Regular.ttf\nINTER_DISPLAY_LIGHT=${INTER_DIR}/InterDisplay-Light.ttf\n")
add_custom_command(
  OUTPUT "${fonts_cpp}"
  COMMAND "${CMAKE_COMMAND}" -DOUT=${fonts_cpp} -DLIST=${fonts_list} -P "${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake"
  DEPENDS ${font_files} "${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake"
  COMMENT "Building the HUD's fonts into C++")

file(GLOB_RECURSE hud_files CONFIGURE_DEPENDS hud/*.cpp)
add_executable(SessionScopeHUD ${hud_files} "${fonts_cpp}")
target_include_directories(SessionScopeHUD PRIVATE hud)
target_include_directories(SessionScopeHUD SYSTEM PRIVATE "${STB_DIR}")
# (glfw listed too: an object library's compiled files are not passed on through another one)
target_link_libraries(SessionScopeHUD PRIVATE engine imgui glfw)
target_compile_options(SessionScopeHUD PRIVATE ${OUR_WARNINGS})
set_target_properties(SessionScopeHUD PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SESSIONSCOPE_OUT}")
if(APPLE)
  target_link_libraries(SessionScopeHUD PRIVATE "-framework Cocoa" "-framework IOKit" "-framework OpenGL"
                                                "-framework CoreFoundation" "-framework QuartzCore")
else()
  # Direct3D 11, DXGI and DirectComposition for drawing (window/renderer_d3d11.cpp), the usual
  # Windows libraries for windows and timers, and "windows" subsystem: no console window.
  target_link_libraries(SessionScopeHUD PRIVATE d3d11 dxgi dcomp d3dcompiler_47 gdi32 user32 shell32 winmm
                                                -Wl,--subsystem,windows)
endif()

# ---- the tests: one program each, run from the repository folder (some read engine/tests/golden)
file(GLOB test_files CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(test_file IN LISTS test_files)
  get_filename_component(name "${test_file}" NAME_WE)
  add_executable(${name} "${test_file}")
  target_link_libraries(${name} PRIVATE engine)
  target_compile_options(${name} PRIVATE ${OUR_WARNINGS})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SESSIONSCOPE_OUT}/tests")
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()
