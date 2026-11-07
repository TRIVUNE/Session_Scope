#ifndef SESSIONSCOPE_HUD_GL_H
#define SESSIONSCOPE_HUD_GL_H

// =================================================================================================
// gl.h - the system's OpenGL header, included the right way on each system
// =================================================================================================
//
// Only renderer_gl.cpp (macOS) uses it. It calls a handful of old, basic OpenGL functions itself
// (textures, clearing, reading pixels); Dear ImGui's OpenGL backend loads the newer ones it needs.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX            // keep windows.h from defining min/max macros
#endif
#include <windows.h>        // the Windows OpenGL header needs this first
#include <GL/gl.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION   // Apple marks OpenGL "deprecated"; it still ships and works
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>          // Linux and others
#endif

#endif  // SESSIONSCOPE_HUD_GL_H
