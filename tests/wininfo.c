/* Smoke-test helper: prints every visible top-level window with its rect.
 * x86_64-w64-mingw32-gcc -municode tests/wininfo.c -o build/wininfo.exe */
#define UNICODE
#include <windows.h>
#include <stdio.h>

static BOOL CALLBACK cb(HWND h, LPARAM lp)
{
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[128], title[128];
    RECT r;
    GetClassNameW(h, cls, 128);
    GetWindowTextW(h, title, 128);
    GetWindowRect(h, &r);
    wprintf(L"%p %-28ls %-24ls %5ld,%5ld %5ldx%5ld\n", (void *)h, cls, title, r.left, r.top,
            r.right - r.left, r.bottom - r.top);
    return TRUE;
}

int wmain(void)
{
    EnumWindows(cb, 0);
    return 0;
}
