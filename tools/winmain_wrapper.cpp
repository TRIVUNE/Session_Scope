// Entry point for building Steinberg's validator with Zig (tools/run_vst3_validator.py).
// The validator only defines wmain, but Zig's MinGW startup code looks for WinMain, so this
// provides one that calls it.

#include <windows.h>
#include <shellapi.h>

extern "C" int wmain(int argc, wchar_t* argv[]);

extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int result = wmain(argc, argv);
    LocalFree(argv);
    return result;
}
