#ifndef SESSIONSCOPE_HUD_GL_H
#define SESSIONSCOPE_HUD_GL_H

// The system's OpenGL header. The HUD only uses OpenGL 1.1 calls itself (textures, clear, read
// pixels); Dear ImGui's backend loads the newer functions it needs on its own.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // the Windows GL header needs it first
#include <GL/gl.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION   // Apple marks OpenGL deprecated; it still ships and works
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#endif  // SESSIONSCOPE_HUD_GL_H
