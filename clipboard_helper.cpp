// clipboard_helper.cpp
// Pure Win32 clipboard access.  Must NOT include raylib.h.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include "clipboard_helper.h"

void Clipboard_Set(const char* text) {
    if (!text) return;
    size_t len = strlen(text) + 1;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hg) return;
    memcpy(GlobalLock(hg), text, len);
    GlobalUnlock(hg);
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, hg);
        CloseClipboard();
    } else {
        GlobalFree(hg);
    }
}

char* Clipboard_Get(void) {
    char* out = nullptr;
    if (!OpenClipboard(nullptr)) return nullptr;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char* p = (const char*)GlobalLock(h);
        if (p) {
            size_t len = strlen(p) + 1;
            out = (char*)malloc(len);
            if (out) memcpy(out, p, len);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}
