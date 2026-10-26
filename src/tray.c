/* phiwm — tray icon (shows the focused workspace number), menu, autostart */
#include "phiwm.h"
#include <shellapi.h>
#include <math.h>
#include <stdio.h>

#define ID_LOG       1
#define ID_AUTOSTART 2
#define ID_QUIT      3

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static NOTIFYICONDATAW nid;
static HICON icon;
static int shown_ws = -1;
static unsigned shown_color = 0xFFFFFFFF;
static int added;

UINT tray_taskbar_created_msg(void)
{
    static UINT m;
    if (!m) m = RegisterWindowMessageW(L"TaskbarCreated");
    return m;
}

static unsigned tray_color(void)
{
    return PHI_ACTIVE_BORDER;
}

/* Draws the workspace number on a rounded square in the accent color. */
static HICON make_icon(int number, unsigned rgb)
{
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (size < 16) size = 16;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!dib || !bits) { DeleteDC(dc); return NULL; }
    HGDIOBJ oldbmp = SelectObject(dc, dib);
    ZeroMemory(bits, (size_t)size * size * 4);

    wchar_t txt[8];
    wsprintfW(txt, L"%d", number);
    int fh = number >= 10 ? size * 62 / 100 : size * 80 / 100;
    HFONT font = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldfont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = { 0, 0, size, size };
    DrawTextW(dc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    int br = (rgb >> 16) & 255, bgc = (rgb >> 8) & 255, bb = rgb & 255;
    int lum = (br * 299 + bgc * 587 + bb * 114) / 1000;
    int fg = lum > 150 ? 20 : 255;
    double rad = size / 4.0;
    unsigned char *px = bits;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            unsigned char *p = px + ((size_t)y * size + x) * 4;
            double cov = p[2] / 255.0;
            double cx = x + 0.5, cy = y + 0.5;
            double dx = fmax(0.0, fmax(rad - cx, cx - (size - rad)));
            double dy = fmax(0.0, fmax(rad - cy, cy - (size - rad)));
            double a = rad - sqrt(dx * dx + dy * dy) + 0.5;
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            p[0] = (unsigned char)(bb + (fg - bb) * cov);
            p[1] = (unsigned char)(bgc + (fg - bgc) * cov);
            p[2] = (unsigned char)(br + (fg - br) * cov);
            p[3] = (unsigned char)(a * 255);
        }
    }

    SelectObject(dc, oldfont);
    DeleteObject(font);
    SelectObject(dc, oldbmp);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, dib };
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(dib);
    return h;
}

static void set_tip(int ws)
{
    _snwprintf(nid.szTip, ARRAYSIZE(nid.szTip), L"phiwm — workspace %d", ws);
    nid.szTip[ARRAYSIZE(nid.szTip) - 1] = 0;
}

void tray_add(void)
{
    int ws = wm_focused_ws_number();
    unsigned col = tray_color();
    HICON h = make_icon(ws, col);
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g.msgwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_PHI_TRAY;
    nid.hIcon = h ? h : LoadIconW(g.inst, MAKEINTRESOURCEW(1));
    set_tip(ws);
    added = Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    if (icon) DestroyIcon(icon);
    icon = h;
    shown_ws = ws;
    shown_color = col;
}

void tray_update(void)
{
    if (!added) return;
    int ws = wm_focused_ws_number();
    unsigned col = tray_color();
    if (ws == shown_ws && col == shown_color) return;
    HICON h = make_icon(ws, col);
    if (!h) return;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = h;
    set_tip(ws);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (icon) DestroyIcon(icon);
    icon = h;
    shown_ws = ws;
    shown_color = col;
}

void tray_remove(void)
{
    if (added) Shell_NotifyIconW(NIM_DELETE, &nid);
    added = 0;
    if (icon) DestroyIcon(icon);
    icon = NULL;
}

int autostart_get(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExW(k, L"phiwm", NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

void autostart_set(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", exe);
        cmd[ARRAYSIZE(cmd) - 1] = 0;
        RegSetValueExW(k, L"phiwm", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"phiwm");
    }
    RegCloseKey(k);
}

void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"phiwm " TEXT(PHI_VERSION_STR));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit (shows all windows)");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.msgwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.msgwnd, NULL);
    PostMessageW(g.msgwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case ID_LOG: wm_open_log(); break;
    case ID_AUTOSTART: autostart_set(!autostart_get()); break;
    case ID_QUIT: wm_quit(); break;
    }
}
