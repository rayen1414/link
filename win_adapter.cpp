#include "win_adapter.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void Win_SendMouseClick(int x, int y, bool isLeft) {
    SetCursorPos(x, y);
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = isLeft ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &input, sizeof(INPUT));
    
    input.mi.dwFlags = isLeft ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
    SendInput(1, &input, sizeof(INPUT));
}