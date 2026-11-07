# =================================================================================================
# third_party.cmake - the libraries the HUD is built on, downloaded once and compiled here
# =================================================================================================
#
#   GLFW 3.4           (zlib licence)  github.com/glfw/glfw         the window, mouse, keyboard
#   Dear ImGui 1.91.9  (MIT licence)   github.com/ocornut/imgui     drawing boxes, text, menus
#   stb_image_write    (public domain) github.com/nothings/stb      PNG screenshots
#   Inter 4.1          (SIL OFL)       github.com/rsms/inter        the font
#
# Their own warnings are switched off (-w): they're not our code, and our code is built with every
# warning turned into an error.

fetch_git(glfw https://github.com/glfw/glfw 3.4 7b6aead9fb88b3623e3b3725ebb42670cbe4c579)
fetch_git(imgui https://github.com/ocornut/imgui v1.91.9 97428e8ac99e339ce05eee531cf55b77b29ea709)
fetch_file(stb/stb_image_write.h
           https://raw.githubusercontent.com/nothings/stb/d14d46f6b44456eb88beac58cba833a4135d4c89/stb_image_write.h
           cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05)
fetch_from_zip(inter https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
               9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
               extras/ttf/InterDisplay-Light.ttf extras/ttf/Inter-Regular.ttf LICENSE.txt)

set(GLFW_DIR "${TOOLCHAIN_DIR}/glfw")
set(IMGUI_DIR "${TOOLCHAIN_DIR}/imgui")
set(STB_DIR "${TOOLCHAIN_DIR}/stb")
set(INTER_DIR "${TOOLCHAIN_DIR}/inter")

# ---- GLFW: only the source files for this system (its own CMake build is not used).
set(glfw_files context.c init.c input.c monitor.c platform.c vulkan.c window.c egl_context.c osmesa_context.c
               null_init.c null_monitor.c null_window.c null_joystick.c)
if(APPLE)
  enable_language(OBJC)
  list(APPEND glfw_files cocoa_time.c posix_thread.c posix_module.c cocoa_init.m cocoa_joystick.m cocoa_monitor.m
                         cocoa_window.m nsgl_context.m)
else()
  list(APPEND glfw_files win32_time.c win32_thread.c win32_module.c win32_init.c win32_joystick.c win32_monitor.c
                         win32_window.c wgl_context.c)
endif()
list(TRANSFORM glfw_files PREPEND "${GLFW_DIR}/src/")
add_library(glfw OBJECT ${glfw_files})
target_include_directories(glfw SYSTEM PUBLIC "${GLFW_DIR}/include" PRIVATE "${GLFW_DIR}/src")
target_compile_definitions(glfw PRIVATE $<IF:$<BOOL:${APPLE}>,_GLFW_COCOA,_GLFW_WIN32>)
target_compile_options(glfw PRIVATE -w)
set_target_properties(glfw PROPERTIES C_STANDARD 11)

# ---- Dear ImGui: its core, the GLFW input code, and the drawing code for this system
# (Direct3D 11 on Windows, OpenGL 3 on macOS: engine/hud/window/renderer_*.cpp).
set(imgui_files imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_glfw.cpp)
if(APPLE)
  list(APPEND imgui_files backends/imgui_impl_opengl3.cpp)
else()
  list(APPEND imgui_files backends/imgui_impl_dx11.cpp)
endif()
list(TRANSFORM imgui_files PREPEND "${IMGUI_DIR}/")
add_library(imgui OBJECT ${imgui_files})
target_include_directories(imgui SYSTEM PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(imgui PUBLIC glfw)
target_compile_options(imgui PRIVATE -w)
