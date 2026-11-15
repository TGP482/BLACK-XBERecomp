#pragma once
// Linux builds: the Win32 names portable game code uses as plain values. Window messages are what the SDL host
// (app/main_sdl.cpp) sends GameInfo::message; virtual key codes are the games' key identifiers, and the host sends
// them by key position, as a US keyboard has them. No Win32 behaviour lives here.
#include <strings.h>  // strcasecmp, which the build names _stricmp

#include <chrono>
#include <cstdint>
#include <thread>

typedef int32_t HRESULT;  // 4 bytes as on Windows, so FAILED() means the same
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;
typedef void* HCURSOR;
typedef const wchar_t* PCWSTR;
typedef unsigned int UINT;
typedef uint32_t DWORD;
typedef int BOOL;
#define WINAPI
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

enum : uint32_t {
    WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_ACTIVATE = 0x0006, WM_CLOSE = 0x0010, WM_SETCURSOR = 0x0020,
    WM_INPUT = 0x00FF, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104,
    WM_SYSKEYUP = 0x0105, WM_SYSCOMMAND = 0x0112, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
    WM_APP = 0x8000,
};
enum { WA_INACTIVE = 0, WA_ACTIVE = 1, HTCLIENT = 1, SC_KEYMENU = 0xF100 };

enum : int {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8,
    VK_NUMPAD9, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SEPARATOR = 0x6C, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E,
    VK_DIVIDE = 0x6F,
    VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5,
    VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE,
    VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
    VK_OEM_102 = 0xE2,
};

#define LOWORD(v) ((uint16_t)((uintptr_t)(v) & 0xFFFF))
#define HIWORD(v) ((uint16_t)(((uintptr_t)(v) >> 16) & 0xFFFF))
#define MAKEINTRESOURCEW(i) ((const wchar_t*)(uintptr_t)(uint16_t)(i))

#ifndef XBE_COMPAT_SLEEP
#define XBE_COMPAT_SLEEP  // winsock2.h's compat may have it already
inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
