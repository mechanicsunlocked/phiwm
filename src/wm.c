/* phiwm — window manager core: tracking, workspaces, monitors, dispatchers.
 *
 * Everything runs on the main thread. Window changes arrive as out-of-context
 * WinEvents; keybinds arrive as WM_PHI_BIND from the hook thread. Calls into
 * other processes' windows are asynchronous (ShowWindowAsync,
 * SWP_ASYNCWINDOWPOS) so a hung app can never freeze the tiler.
 */
#include "phiwm.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Globals g;

/* DWM attributes by value, so older headers are fine */
#define A_EXTENDED_FRAME_BOUNDS 9
#define A_CLOAKED               14
#define A_CORNER                33
#define A_BORDER_COLOR          34
#define A_TRANSITIONS_OFF       3    /* DWMWA_TRANSITIONS_FORCEDISABLED */
#define COLOR_DEFAULT           0xFFFFFFFFu

enum {
    T_RETILE = 1, T_SETTLE1, T_SETTLE2, T_FOCUS, T_REHOOK, T_MONITORS, T_MOUSE
};

#define COPYDATA_MAGIC 0x50484957u   /* 'PHIW' */

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int scale(int v, UINT dpi) { return MulDiv(v, dpi ? (int)dpi : 96, 96); }
static ULONGLONG now(void) { return GetTickCount64(); }

static Win *win_find(HWND h)
{
    if (!h) return NULL;
    for (int i = 0; i < g.nwins; i++)
        if (g.wins[i]->hwnd == h) return g.wins[i];
    return NULL;
}

static int ws_visible(int i)
{
    if (i < 0 || i >= PHI_MAX_WS) return 0;
    int m = g.ws[i].mon;
    return m >= 0 && m < g.nmon && g.mon[m].ws == i;
}

static int ws_is_empty(int i)
{
    for (int k = 0; k < g.nwins; k++)
        if (g.wins[k]->ws == i) return 0;
    return 1;
}

static int cur_ws(void) { return g.nmon ? g.mon[g.cur_mon].ws : 0; }

int wm_focused_ws_number(void) { return cur_ws() + 1; }

static Win *ws_mru(int i, Win *exclude, int tiled_only)
{
    Win *best = NULL;
    for (int k = 0; k < g.nwins; k++) {
        Win *w = g.wins[k];
        if (w->ws != i || w == exclude || w->minimized) continue;
        if (tiled_only && !w->node) continue;
        if (!best || w->focus_stamp > best->focus_stamp) best = w;
    }
    return best;
}

static PhiRect ws_area(int i, int *gap_in)
{
    int m = g.ws[i].mon;
    if (m < 0 || m >= g.nmon) m = 0;
    Monitor *mo = &g.mon[m];
    int go = scale(PHI_GAPS_OUT, mo->dpi);
    if (gap_in) *gap_in = scale(PHI_GAPS_IN, mo->dpi);
    PhiRect r = { mo->work.left + go, mo->work.top + go,
                  (mo->work.right - mo->work.left) - 2 * go,
                  (mo->work.bottom - mo->work.top) - 2 * go };
    if (r.w < 100) r.w = 100;
    if (r.h < 100) r.h = 100;
    return r;
}

static int mon_from_point(POINT p)
{
    HMONITOR h = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < g.nmon; i++) if (g.mon[i].h == h) return i;
    return 0;
}

static int mon_from_hwnd(HWND hwnd)
{
    HMONITOR h = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < g.nmon; i++) if (g.mon[i].h == h) return i;
    return 0;
}

static COLORREF colorref(unsigned rgb)
{
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

/* ------------------------------------------------------------------ */
/* window classification                                               */
/* ------------------------------------------------------------------ */

static const wchar_t *ignored_classes[] = {
    L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
    L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"ForegroundStaging",
    L"MultitaskingViewFrame", L"TaskSwitcherWnd", L"NotifyIconOverflowWindow",
    L"TopLevelWindowForOverflowXamlIsland", L"Shell_InputSwitchTopLevelWindow",
    L"LauncherTipWnd", L"tooltips_class32", L"#32768", L"Xaml_WindowedPopupClass",
    L"PseudoConsoleWindow", L"SysShadow", L"EdgeUiInputTopWndClass",
    L"ApplicationManager_ImmersiveShellWindow", L"Internet Explorer_Hidden",
    L"Windows.Internal.Shell.TabProxyWindow", L"DV2ControlHost", L"MsgrIMEWindowClass",
    PHI_CLASS_MSG, NULL
};

static int class_ignored(const wchar_t *cls)
{
    for (int i = 0; ignored_classes[i]; i++)
        if (!lstrcmpW(cls, ignored_classes[i])) return 1;
    return 0;
}

static int is_cloaked(HWND h)
{
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, A_CLOAKED, &c, sizeof c)) && c;
}

static int process_elevated(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    int elevated = 0;
    HANDLE tok;
    if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te;
        DWORD n;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof te, &n)) elevated = te.TokenIsElevated != 0;
        CloseHandle(tok);
    } else {
        elevated = 1;
    }
    CloseHandle(p);
    return elevated;
}

typedef struct { DWORD pid; DWORD found; } ChildPid;

static BOOL CALLBACK find_uwp_child(HWND h, LPARAM lp)
{
    ChildPid *c = (ChildPid *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid && pid != c->pid) { c->found = pid; return FALSE; }
    return TRUE;
}

/* exe file name of the process behind a window; UWP apps report the real app, not ApplicationFrameHost */
static void window_exe(HWND h, const wchar_t *cls, char *out, int n)
{
    out[0] = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!lstrcmpW(cls, L"ApplicationFrameWindow")) {
        ChildPid c = { pid, 0 };
        EnumChildWindows(h, find_uwp_child, (LPARAM)&c);
        if (c.found) pid = c.found;
    }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &len)) {
        wchar_t *base = path;
        for (wchar_t *q = path; *q; q++) if (*q == L'\\') base = q + 1;
        utf8_from_wide(base, out, n);
    }
    CloseHandle(p);
}

/* Returns 1 if the window should be managed; *float_hint says it should float. */
static int eligible(HWND h, int *float_hint, wchar_t *cls, int ncls)
{
    cls[0] = 0;
    if (!h || h == g.msgwnd || !IsWindow(h) || !IsWindowVisible(h)) return 0;
    if (GetAncestor(h, GA_ROOT) != h) return 0;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (st & WS_CHILD) return 0;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return 0;
    int has_caption = (st & WS_CAPTION) == WS_CAPTION;
    int sizable = (st & WS_THICKFRAME) != 0;
    if (!has_caption && !sizable) return 0;
    HWND owner = GetWindow(h, GW_OWNER);
    if (owner && (IsWindowVisible(owner) || win_find(owner))) return 0;   /* dialogs float on their own */
    if (is_cloaked(h)) return 0;
    GetClassNameW(h, cls, ncls);
    if (class_ignored(cls)) return 0;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 40 || r.bottom - r.top < 40) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return 0;
    if (!g.self_elevated && process_elevated(pid)) return 0;  /* Windows won't let us move it */
    *float_hint = !sizable || (ex & WS_EX_TOPMOST) || (ex & WS_EX_DLGMODALFRAME) || !lstrcmpW(cls, L"#32770");
    return 1;
}

static void strip_exe(const char *exe, char *out, size_t n)
{
    size_t l = strlen(exe);
    if (l >= n) l = n - 1;
    memcpy(out, exe, l);
    out[l] = 0;
    if (l > 4 && !_stricmp(out + l - 4, ".exe")) out[l - 4] = 0;
}

static int rule_matches(const PhiRule *r, const char *cls8, const char *exe, const char *title8)
{
    char base[MAX_PATH];
    strip_exe(exe, base, sizeof base);
    switch (r->field) {
    case PHI_FIELD_CLASS: return phi_pattern_match(r->pat, cls8) || phi_pattern_match(r->pat, base);
    case PHI_FIELD_TITLE: return phi_pattern_match(r->pat, title8);
    case PHI_FIELD_EXE:   return phi_pattern_match(r->pat, exe) || phi_pattern_match(r->pat, base);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* decoration: native Windows 11 border color and corners             */
/* ------------------------------------------------------------------ */

static void set_border(Win *w, int state)
{
    if (!w || w->border_state == state) return;
    COLORREF c = COLOR_DEFAULT;
    if (state == 2) c = colorref(PHI_ACTIVE_BORDER);
    if (state == 1) c = colorref(PHI_INACTIVE_BORDER);
    DwmSetWindowAttribute(w->hwnd, A_BORDER_COLOR, &c, sizeof c);
    w->border_state = state;
}

static void set_corners(Win *w, int reset)
{
    DWORD pref = (reset || PHI_ROUNDING) ? 0 /* default */ : 1 /* do not round */;
    DwmSetWindowAttribute(w->hwnd, A_CORNER, &pref, sizeof pref);
}

/* DWM fades a window in and out when it is shown or hidden. Off for managed windows,
 * so a workspace switch is instant. Per window, nothing system-wide. */
static void set_transitions(HWND h, int off)
{
    BOOL v = off ? TRUE : FALSE;
    DwmSetWindowAttribute(h, A_TRANSITIONS_OFF, &v, sizeof v);
}

/* Fullscreen 0: no caption, no frame, square corners, and the window rect is the
 * monitor, so the shell treats it as a fullscreen app and keeps the taskbar under
 * it. The style change is the one synchronous call phiwm makes into another
 * process; the user just asked for it on the active window, so it is awake. */
static void fs_style(Win *w, int on)
{
    HWND h = w->hwnd;
    if (!IsWindow(h)) { w->fs_styled = 0; return; }
    if (on && !w->fs_styled) {
        w->fs_style = GetWindowLongW(h, GWL_STYLE);
        w->fs_exstyle = GetWindowLongW(h, GWL_EXSTYLE);
        w->fs_styled = 1;
        SetWindowLongW(h, GWL_STYLE, w->fs_style & ~(LONG)WS_OVERLAPPEDWINDOW);
        SetWindowLongW(h, GWL_EXSTYLE, w->fs_exstyle &
                       ~(LONG)(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        DWORD square = 1;   /* DWMWCP_DONOTROUND */
        DwmSetWindowAttribute(h, A_CORNER, &square, sizeof square);
    } else if (!on && w->fs_styled) {
        w->fs_styled = 0;
        SetWindowLongW(h, GWL_STYLE, w->fs_style);
        SetWindowLongW(h, GWL_EXSTYLE, w->fs_exstyle);
        set_corners(w, 0);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
    }
}

/* Leaves fullscreen: the window gets its frame back. The caller retiles. */
static void fs_clear(Workspace *ws)
{
    Win *f = ws->fs;
    if (!f) return;
    ws->fs = NULL;
    if (f->fs_styled) fs_style(f, 0);
}

static void mark_active(Win *w)
{
    if (g.active_border_win == w) return;
    if (g.active_border_win) set_border(g.active_border_win, 1);
    g.active_border_win = w;
    if (w) set_border(w, 2);
}

/* ------------------------------------------------------------------ */
/* visibility                                                          */
/* ------------------------------------------------------------------ */

static void hidden_list_path(wchar_t *out)
{
    _snwprintf(out, MAX_PATH, L"%ls\\hidden.txt", g.data_dir);
    out[MAX_PATH - 1] = 0;
}

/* One line per window phiwm has hidden: "hwnd pid class". HWNDs only use
 * 32 significant bits, so plain %lx is enough and portable across CRTs. */
static void save_hidden_list(void)
{
    wchar_t path[MAX_PATH];
    hidden_list_path(path);
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    for (int i = 0; i < g.nwins; i++) {
        Win *w = g.wins[i];
        if (!w->hidden) continue;
        for (int k = -1; k < w->nowned; k++) {
            HWND hw = k < 0 ? w->hwnd : w->owned[k];
            DWORD pid = 0;
            wchar_t cls[128];
            char cls8[256];
            GetWindowThreadProcessId(hw, &pid);
            GetClassNameW(hw, cls, 128);
            utf8_from_wide(cls, cls8, sizeof cls8);
            fprintf(f, "%lx %lu %s\n", (unsigned long)(ULONG_PTR)hw, (unsigned long)pid, cls8);
        }
    }
    fclose(f);
}

/* If a previous run died with windows hidden, bring them back. */
static void restore_hidden_from_file(void)
{
    wchar_t path[MAX_PATH];
    hidden_list_path(path);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char line[400];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *end;
        unsigned long hv = strtoul(line, &end, 16);
        unsigned long pid = strtoul(end, &end, 10);
        while (*end == ' ') end++;
        end[strcspn(end, "\r\n")] = 0;
        if (!hv) continue;
        HWND h = (HWND)(ULONG_PTR)hv;
        if (!IsWindow(h) || IsWindowVisible(h)) continue;
        DWORD p2 = 0;
        wchar_t now_cls[200], cls[200];
        GetWindowThreadProcessId(h, &p2);
        GetClassNameW(h, now_cls, 200);
        wide_from_utf8(end, cls, 200);
        if (p2 == pid && !lstrcmpW(now_cls, cls)) { ShowWindowAsync(h, SW_SHOWNA); n++; }
    }
    fclose(f);
    DeleteFileW(path);
    if (n) phi_log("restored %d window(s) hidden by a previous run", n);
}

static BOOL CALLBACK collect_owned(HWND h, LPARAM lp)
{
    Win *w = (Win *)lp;
    if (w->nowned >= 16 || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != w->hwnd) return TRUE;
    wchar_t cls[128];
    GetClassNameW(h, cls, 128);
    /* ConPTY's pseudo console window is owned by the terminal window. Hiding it tells
     * the terminal its console app wants to hide, and Windows Terminal then minimizes
     * itself. Nothing in the ignore list needs hiding anyway. */
    if (class_ignored(cls)) return TRUE;
    w->owned[w->nowned++] = h;
    return TRUE;
}

/* SW_HIDE doesn't hide the windows a window owns (Find boxes, palettes, Save As),
 * so they go with it. */
static void win_hide(Win *w)
{
    if (w->hidden) return;
    w->hidden = 1;
    w->vis_tick = now();
    w->nowned = 0;
    EnumWindows(collect_owned, (LPARAM)w);
    for (int i = 0; i < w->nowned; i++) ShowWindowAsync(w->owned[i], SW_HIDE);
    ShowWindowAsync(w->hwnd, SW_HIDE);
}

static void show_owned(Win *w)
{
    for (int i = 0; i < w->nowned; i++)
        if (IsWindow(w->owned[i]) && GetWindow(w->owned[i], GW_OWNER) == w->hwnd)
            ShowWindowAsync(w->owned[i], SW_SHOWNA);
    w->nowned = 0;
}

static void win_show(Win *w)
{
    if (!w->hidden) return;
    w->hidden = 0;
    w->vis_tick = now();
    ShowWindowAsync(w->hwnd, SW_SHOWNA);
    show_owned(w);
}

/* ------------------------------------------------------------------ */
/* placement                                                           */
/* ------------------------------------------------------------------ */

static void read_margins(Win *w)
{
    HWND h = w->hwnd;
    if (!IsWindowVisible(h) || IsIconic(h) || IsZoomed(h)) return;
    RECT wr, fr;
    if (!GetWindowRect(h, &wr)) return;
    if (FAILED(DwmGetWindowAttribute(h, A_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr))) return;
    RECT m = { fr.left - wr.left, fr.top - wr.top, wr.right - fr.right, wr.bottom - fr.bottom };
    if (m.left < 0 || m.left > 40 || m.top < 0 || m.top > 40 || m.right < 0 || m.right > 40 ||
        m.bottom < 0 || m.bottom > 40) return;
    /* a resizable frame always has invisible borders; all zero means DWM has not
     * caught up with a frame change yet (leaving fullscreen): keep the known values */
    if (!m.left && !m.top && !m.right && !m.bottom && (GetWindowLongW(h, GWL_STYLE) & WS_THICKFRAME)) return;
    w->margin = m;
    w->has_margin = 1;
}

/* The smallest outer size the app accepts (WM_GETMINMAXINFO), as the visible rect in
 * physical pixels of its monitor. The wait is bounded; a hung app gets no minimum. */
static void read_min_size(Win *w)
{
    w->min_w = w->min_h = 0;
    MINMAXINFO mmi;
    ZeroMemory(&mmi, sizeof mmi);
    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(w->hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mmi, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &res))
        return;
    UINT mdpi = g.mon[mon_from_hwnd(w->hwnd)].dpi;
    UINT wdpi = GetDpiForWindow(w->hwnd);   /* 96 for DPI-unaware apps: Windows scales them up */
    if (!wdpi) wdpi = mdpi;
    int mw = MulDiv(mmi.ptMinTrackSize.x, (int)mdpi, (int)wdpi);
    int mh = MulDiv(mmi.ptMinTrackSize.y, (int)mdpi, (int)wdpi);
    if (w->has_margin) { mw -= w->margin.left + w->margin.right; mh -= w->margin.top + w->margin.bottom; }
    /* Windows' own default (a title bar's worth) is not a constraint worth tracking */
    if (mw > GetSystemMetricsForDpi(SM_CXMINTRACK, mdpi)) w->min_w = mw;
    if (mh > GetSystemMetricsForDpi(SM_CYMINTRACK, mdpi)) w->min_h = mh;
}

/* Puts the visible part of the window exactly on `v`. With `reveal` the window is
 * also shown in the same call, so it can never be seen at a stale spot first.
 * Returns 1 if it moved it. */
static int place(Win *w, PhiRect v, int reveal)
{
    HWND h = w->hwnd;
    w->tile = v;
    if (IsIconic(h)) return 0;
    int zoomed = IsZoomed(h);
    if (zoomed) ShowWindowAsync(h, SW_SHOWNOACTIVATE);
    else read_margins(w);
    RECT m = w->has_margin ? w->margin : (RECT){ 0, 0, 0, 0 };
    RECT want = { v.x - m.left, v.y - m.top, v.x + v.w + m.right, v.y + v.h + m.bottom };
    RECT cur;
    w->placed = want;
    w->has_placed = 1;
    if (!reveal && !zoomed && GetWindowRect(h, &cur) && EqualRect(&cur, &want)) return 0;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
    if (reveal) flags |= SWP_SHOWWINDOW;
    SetWindowPos(h, NULL, want.left, want.top, want.right - want.left, want.bottom - want.top, flags);
    return 1;
}

/* Bookkeeping after place(w, r, 1). Minimized windows are not placed, so they are
 * shown the plain way (they stay minimized, back in the taskbar). */
static void revealed(Win *w)
{
    if (!w->hidden) return;
    w->hidden = 0;
    w->vis_tick = now();
    if (IsIconic(w->hwnd)) ShowWindowAsync(w->hwnd, SW_SHOWNA);
    show_owned(w);
}

static void place_floating_on_monitor(Win *w, int m)
{
    RECT r;
    if (!GetWindowRect(w->hwnd, &r)) return;
    POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    if (MonitorFromPoint(c, MONITOR_DEFAULTTONULL) == g.mon[m].h) return;
    RECT wa = g.mon[m].work;
    int ww = r.right - r.left, hh = r.bottom - r.top;
    if (ww > wa.right - wa.left) ww = wa.right - wa.left;
    if (hh > wa.bottom - wa.top) hh = wa.bottom - wa.top;
    int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - hh) / 2;
    SetWindowPos(w->hwnd, NULL, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

/* Lays out workspace i. With `reveal`, hidden tiled windows are shown as they are placed. */
static void apply_ws_ex(int i, int reveal)
{
    if (!ws_visible(i)) return;
    Workspace *ws = &g.ws[i];
    Monitor *mo = &g.mon[ws->mon];
    int gin;
    PhiRect area = ws_area(i, &gin);
    for (PhiNode *n = phi_first(&ws->tree); n; n = phi_next(n)) {
        Win *w = n->win;   /* its minimum plus the gap it may lose on either side */
        phi_set_min(n, w->min_w ? w->min_w + 2 * gin : 0, w->min_h ? w->min_h + 2 * gin : 0);
    }
    phi_layout(&ws->tree, area, &phi_dwindle);
    for (PhiNode *n = phi_first(&ws->tree); n; n = phi_next(n)) {
        Win *w = n->win;
        if (w == ws->fs) continue;
        PhiRect r = phi_tile_rect(n, area, gin);
        if (w->pseudo && w->has_float_rect) {
            int m_lr = w->has_margin ? w->margin.left + w->margin.right : 0;
            int m_tb = w->has_margin ? w->margin.top + w->margin.bottom : 0;
            int fw = (w->float_rect.right - w->float_rect.left) - m_lr;
            int fh = (w->float_rect.bottom - w->float_rect.top) - m_tb;
            if (fw > 0 && fw < r.w) { r.x += (r.w - fw) / 2; r.w = fw; }
            if (fh > 0 && fh < r.h) { r.y += (r.h - fh) / 2; r.h = fh; }
        }
        place(w, r, reveal && w->hidden);
        if (reveal) revealed(w);
    }
    if (ws->fs && ws->fs_mode != 2) {
        Win *f = ws->fs;
        if (ws->fs_mode == 0) {
            /* the exact monitor rect, no margins: the frame is gone */
            RECT rc = mo->rc;
            if (!f->fs_styled) fs_style(f, 1);
            if (IsZoomed(f->hwnd)) ShowWindowAsync(f->hwnd, SW_SHOWNOACTIVATE);
            f->tile = (PhiRect){ rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top };
            f->placed = rc;
            f->has_placed = 1;
            UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS;
            if (reveal && f->hidden) flags |= SWP_SHOWWINDOW;
            SetWindowPos(f->hwnd, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, flags);
        } else {
            place(f, area, reveal && f->hidden);
            SetWindowPos(f->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        if (reveal) revealed(f);
    }
}

static void apply_ws(int i) { apply_ws_ex(i, 0); }

static void apply_all(void)
{
    for (int m = 0; m < g.nmon; m++) apply_ws(g.mon[m].ws);
}

static void structural_change(void)
{
    g.epoch++;
}

/* ------------------------------------------------------------------ */
/* focus                                                               */
/* ------------------------------------------------------------------ */

static void force_foreground(HWND h)
{
    if (!h || GetForegroundWindow() == h) return;
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return;
    /* Windows only lets the process that got the last input change focus. Holding Alt
     * lifts that lock; the extra unassigned key stops the Alt release opening a menu bar. */
    if (GetAsyncKeyState(VK_MENU) & 0x8000) { SetForegroundWindow(h); return; }
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_MENU;
    SendInput(1, &in, sizeof in);
    SetForegroundWindow(h);
    in.ki.wVk = 0xE8;
    SendInput(1, &in, sizeof in);
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
    in.ki.wVk = VK_MENU;
    SendInput(1, &in, sizeof in);
}

static POINT mouse_last;   /* where the cursor was at the last focus-follows-mouse check */

/* Like hyprland: when phiwm moves the focus by itself, the cursor goes along, so
 * focus-follows-mouse cannot take the focus straight back on the next nudge. */
static void warp_cursor(Win *w)
{
    RECT r;
    if (w->node) r = (RECT){ w->tile.x, w->tile.y, w->tile.x + w->tile.w, w->tile.y + w->tile.h };
    else if (!IsIconic(w->hwnd)) { if (!GetWindowRect(w->hwnd, &r)) return; }
    else if (w->has_float_rect) r = w->float_rect;
    else return;
    POINT p;
    if (!GetCursorPos(&p) || PtInRect(&r, p)) return;
    mouse_last = (POINT){ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    SetCursorPos(mouse_last.x, mouse_last.y);
}

/* warp = 0 when the mouse brought the focus here itself */
static void focus_win_ex(Win *w, int warp)
{
    if (!w) return;
    if (w->minimized || IsIconic(w->hwnd)) ShowWindowAsync(w->hwnd, SW_RESTORE);
    Workspace *ws = &g.ws[w->ws];
    ws->focus = w;
    w->focus_stamp = ++g.stamp;
    if (ws->mon >= 0) g.cur_mon = ws->mon;
    mark_active(w);
    if (warp) warp_cursor(w);
    force_foreground(w->hwnd);
    if (GetForegroundWindow() != w->hwnd) {
        /* the window may still be processing its async show; try again shortly */
        g.pending_focus = w->hwnd;
        g.focus_tries = 0;
        SetTimer(g.msgwnd, T_FOCUS, 40, NULL);
    }
    tray_update();
}

static void focus_win(Win *w) { focus_win_ex(w, 1); }

static ULONGLONG desktop_focus_tick;

static void focus_desktop_on(int m)
{
    g.cur_mon = m;
    mark_active(NULL);
    desktop_focus_tick = now();
    HWND d = GetShellWindow();
    if (d) force_foreground(d);
    tray_update();
}

/* ------------------------------------------------------------------ */
/* tiling                                                              */
/* ------------------------------------------------------------------ */

static void tile_insert(Win *w, Win *target_win, int force, const POINT *pt)
{
    Workspace *ws = &g.ws[w->ws];
    if (ws->mon < 0) ws->mon = g.cur_mon;
    PhiNode *target = NULL;
    if (target_win && target_win != w && target_win->node && target_win->ws == w->ws) target = target_win->node;
    if (!target && ws->focus && ws->focus != w && ws->focus->node) target = ws->focus->node;
    if (!target) {
        Win *m = ws_mru(w->ws, w, 1);
        if (m) target = m->node;
    }
    POINT p;
    if (pt) p = *pt; else GetCursorPos(&p);
    PhiRect area = ws_area(w->ws, NULL);
    w->node = phi_insert(&ws->tree, target, w, area, &phi_dwindle, force, p.x, p.y);
}

static void tile_remove(Win *w)
{
    if (!w->node) return;
    phi_remove(&g.ws[w->ws].tree, w->node);
    w->node = NULL;
}

/* Swap two tiled windows; works across workspaces/monitors too. */
static void swap_wins(Win *a, Win *b)
{
    if (!a->node || !b->node) return;
    PhiNode *na = a->node, *nb = b->node;
    phi_swap(na, nb);
    a->node = nb;
    b->node = na;
    int wa = a->ws;
    a->ws = b->ws;
    b->ws = wa;
    if (a->ws != b->ws) {
        if (g.ws[a->ws].focus == b) g.ws[a->ws].focus = a;
        if (g.ws[b->ws].focus == a) g.ws[b->ws].focus = b;
        for (int i = 0; i < PHI_MAX_WS; i++)
            if (g.ws[i].fs == a || g.ws[i].fs == b) fs_clear(&g.ws[i]);
    }
    structural_change();
}

/* ------------------------------------------------------------------ */
/* manage / unmanage                                                   */
/* ------------------------------------------------------------------ */

static void switch_ws(int n, Win *focus);

static Win *manage(HWND h, int startup)
{
    if (g.nwins >= PHI_MAX_WIN || win_find(h)) return NULL;
    int float_hint = 0;
    wchar_t cls[128];
    if (!eligible(h, &float_hint, cls, 128)) return NULL;

    Win *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->hwnd = h;
    lstrcpynW(w->cls, cls, 128);
    window_exe(h, cls, w->exe, sizeof w->exe);

    char cls8[256], title8[512];
    wchar_t title[256];
    InternalGetWindowText(h, title, 256);
    utf8_from_wide(cls, cls8, sizeof cls8);
    utf8_from_wide(title, title8, sizeof title8);

    int target = -1, silent = 0;
    for (int i = 0; i < phi_nrules; i++) {
        const PhiRule *r = &phi_rules[i];
        if (!rule_matches(r, cls8, w->exe, title8)) continue;
        switch (r->action) {
        case PHI_RULE_FLOAT: float_hint = 1; break;
        case PHI_RULE_TILE: float_hint = 0; break;
        case PHI_RULE_PSEUDO: w->pseudo = 1; break;
        case PHI_RULE_WORKSPACE: target = r->ws - 1; silent = r->silent; break;
        }
    }
    w->floating = float_hint;
    if (GetWindowRect(h, &w->float_rect)) w->has_float_rect = 1;
    w->minimized = IsIconic(h) != 0;

    if (target < 0) target = (startup || w->floating) ? g.mon[mon_from_hwnd(h)].ws : cur_ws();
    w->ws = target;
    if (g.ws[target].mon < 0) g.ws[target].mon = g.cur_mon;
    w->focus_stamp = ++g.stamp;
    g.wins[g.nwins++] = w;

    read_margins(w);
    read_min_size(w);
    if (!w->floating && !w->minimized) tile_insert(w, NULL, -1, NULL);
    w->border_state = -1;
    set_border(w, 1);
    set_corners(w, 0);
    set_transitions(h, 1);
    structural_change();

    phi_log("manage %p ws=%d float=%d min=%dx%d cls=%s exe=%s title=%s", (void *)h, target + 1,
            w->floating, w->min_w, w->min_h, cls8, w->exe, title8);

    if (!ws_visible(target)) {
        if (!silent && !startup) { switch_ws(target, w); return w; }
        win_hide(w);
        save_hidden_list();
        return w;
    }
    if (!w->floating && !w->minimized && IsZoomed(h)) ShowWindowAsync(h, SW_SHOWNOACTIVATE);
    if (!startup) {
        apply_ws(target);
        SetTimer(g.msgwnd, T_SETTLE1, 150, NULL);
        SetTimer(g.msgwnd, T_SETTLE2, 600, NULL);
        if (!w->minimized) focus_win(w);   /* like hyprland: a new window gets the focus */
    }
    return w;
}

static void unmanage(Win *w, int alive)
{
    int i = w->ws;
    Workspace *ws = &g.ws[i];
    int was_focus = ws->focus == w;
    int was_fg_ws = ws_visible(i) && ws->mon == g.cur_mon;
    tile_remove(w);
    for (int k = 0; k < PHI_MAX_WS; k++) {
        if (g.ws[k].fs == w) fs_clear(&g.ws[k]);
        if (g.ws[k].focus == w) g.ws[k].focus = NULL;
    }
    if (g.dragging == w) g.dragging = NULL;
    if (g.active_border_win == w) g.active_border_win = NULL;
    if (w->hidden) show_owned(w);
    if (alive && IsWindow(w->hwnd)) {
        COLORREF c = COLOR_DEFAULT;
        DwmSetWindowAttribute(w->hwnd, A_BORDER_COLOR, &c, sizeof c);
        set_corners(w, 1);
        set_transitions(w->hwnd, 0);
        if (w->hidden) ShowWindowAsync(w->hwnd, SW_SHOWNA);
    }
    for (int k = 0; k < g.nwins; k++) {
        if (g.wins[k] == w) { g.wins[k] = g.wins[--g.nwins]; break; }
    }
    phi_log("unmanage %p", (void *)w->hwnd);
    free(w);
    structural_change();

    ws->focus = ws_mru(i, NULL, 0);
    if (!ws_visible(i) && ws_is_empty(i)) ws->mon = -1;
    apply_ws(i);
    /* like hyprland: closing a window focuses the next one on the same workspace */
    if (was_focus && was_fg_ws && ws->focus) {
        warp_cursor(ws->focus);
        g.pending_focus = ws->focus->hwnd;
        g.focus_tries = 0;
        SetTimer(g.msgwnd, T_FOCUS, 30, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* workspaces                                                          */
/* ------------------------------------------------------------------ */

static void switch_ws(int n, Win *focus)
{
    if (n < 0 || n >= PHI_MAX_WS || !g.nmon) return;
    Workspace *t = &g.ws[n];
    int before = cur_ws();
    if (ws_visible(n)) {
        if (n != before) g.prev_ws = before;
        g.cur_mon = t->mon;
        Win *f = focus ? focus : t->focus ? t->focus : ws_mru(n, NULL, 0);
        if (f) focus_win(f); else focus_desktop_on(t->mon);
        return;
    }
    int m = (t->mon >= 0 && t->mon < g.nmon) ? t->mon : g.cur_mon;
    int old = g.mon[m].ws;
    g.prev_ws = before;
    t->mon = m;
    g.mon[m].ws = n;
    g.cur_mon = m;
    phi_log("workspace %d -> %d", old + 1, n + 1);
    if (old != n && g.ws[old].fs_mode != 2) fs_clear(&g.ws[old]);   /* leaving puts the frame back */

    apply_ws_ex(n, 1);   /* tiled windows: moved and shown in one step */
    for (int k = 0; k < g.nwins; k++)
        if (g.wins[k]->ws == n && g.wins[k]->floating && !g.wins[k]->minimized)
            place_floating_on_monitor(g.wins[k], m);
    for (int k = 0; k < g.nwins; k++) if (g.wins[k]->ws == n) win_show(g.wins[k]);   /* floating, minimized */
    for (int k = 0; k < g.nwins; k++) if (g.wins[k]->ws == old) win_hide(g.wins[k]);
    if (old != n && ws_is_empty(old)) g.ws[old].mon = -1;

    Win *f = focus ? focus : t->focus ? t->focus : ws_mru(n, NULL, 0);
    if (f) focus_win(f); else focus_desktop_on(m);
    SetTimer(g.msgwnd, T_SETTLE1, 150, NULL);
    save_hidden_list();
    tray_update();
}

/* "3", "+1", "r-1", "e+1", "m+1", "previous", "empty" -> workspace index or -1 */
static int resolve_ws(const char *arg)
{
    int cur = cur_ws();
    while (*arg == ' ') arg++;
    if (!_stricmp(arg, "previous")) return g.prev_ws;
    if (!_stricmp(arg, "empty")) {
        for (int i = 0; i < PHI_MAX_WS; i++) if (ws_is_empty(i) && !ws_visible(i)) return i;
        return -1;
    }
    if ((arg[0] == 'e' || arg[0] == 'm') && (arg[1] == '+' || arg[1] == '-')) {
        int list[PHI_MAX_WS], n = 0, pos = 0;
        for (int i = 0; i < PHI_MAX_WS; i++) {
            if (i == cur) pos = n;
            if (i == cur || !ws_is_empty(i) || ws_visible(i)) list[n++] = i;
        }
        int d = atoi(arg + 1);
        int k = ((pos + d) % n + n) % n;
        return list[k];
    }
    if (arg[0] == 'r' && (arg[1] == '+' || arg[1] == '-')) arg++;
    if (arg[0] == '+' || arg[0] == '-') {
        int k = cur + atoi(arg);
        return k < 0 ? 0 : k >= PHI_MAX_WS ? PHI_MAX_WS - 1 : k;
    }
    int k = atoi(arg) - 1;
    return (k >= 0 && k < PHI_MAX_WS) ? k : -1;
}

enum { MOVE_SILENT, MOVE_FOLLOW, MOVE_KEEP_FOCUS };

/* MOVE_SILENT: focus stays on the old workspace. MOVE_FOLLOW: switch to the new
 * workspace. MOVE_KEEP_FOCUS: the window keeps focus (drag & drop, swapwindow
 * onto another monitor). */
static void move_to_ws(Win *w, int n, int mode, Win *target, const POINT *pt)
{
    if (!w || n < 0 || n >= PHI_MAX_WS) return;
    if (w->ws == n && !target) return;
    int follow = mode == MOVE_FOLLOW;
    if (mode == MOVE_KEEP_FOCUS && !ws_visible(n)) mode = MOVE_SILENT;
    int src = w->ws;
    Workspace *s = &g.ws[src], *d = &g.ws[n];
    tile_remove(w);
    if (s->fs == w) fs_clear(s);
    if (s->focus == w) s->focus = NULL;
    w->ws = n;
    if (d->mon < 0) d->mon = (s->mon >= 0) ? s->mon : g.cur_mon;
    if (!w->floating && !w->minimized) tile_insert(w, target, pt ? 0 : -1, pt);
    d->focus = w;
    w->focus_stamp = ++g.stamp;
    if (!s->focus) s->focus = ws_mru(src, NULL, 0);
    structural_change();

    if (w->floating && ws_visible(n)) place_floating_on_monitor(w, d->mon);

    if (follow) {
        switch_ws(n, w);
        apply_all();
    } else {
        if (ws_visible(n)) { apply_ws_ex(n, 1); win_show(w); }
        else win_hide(w);
        apply_ws(src);
        if (mode == MOVE_KEEP_FOCUS) focus_win(w);
        else if (ws_visible(src) && s->mon == g.cur_mon) {
            if (s->focus) focus_win(s->focus); else focus_desktop_on(s->mon);
        }
        if (!ws_visible(src) && ws_is_empty(src)) s->mon = -1;
        save_hidden_list();
    }
}

/* ------------------------------------------------------------------ */
/* monitors                                                            */
/* ------------------------------------------------------------------ */

typedef struct { Monitor *m; int n; } MonScan;

static BOOL CALLBACK mon_cb(HMONITOR h, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r;
    MonScan *s = (MonScan *)lp;
    if (s->n >= PHI_MAX_MON) return FALSE;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(h, (MONITORINFO *)&mi)) return TRUE;
    Monitor *m = &s->m[s->n++];
    ZeroMemory(m, sizeof *m);
    m->h = h;
    m->rc = mi.rcMonitor;
    m->work = mi.rcWork;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    m->dpi = dx;
    m->ws = -1;
    lstrcpynW(m->name, mi.szDevice, 32);
    if (mi.dwFlags & MONITORINFOF_PRIMARY) m->dpi |= 0x80000000u; /* sort marker, cleared below */
    return TRUE;
}

static int scan_monitors(Monitor *out)
{
    MonScan s = { out, 0 };
    EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)&s);
    /* primary first, then left to right, top to bottom */
    for (int i = 1; i < s.n; i++) {
        for (int j = i; j > 0; j--) {
            Monitor *a = &out[j - 1], *b = &out[j];
            int ap = (a->dpi & 0x80000000u) != 0, bp = (b->dpi & 0x80000000u) != 0;
            int swap = bp && !ap;
            if (ap == bp) swap = b->rc.left < a->rc.left || (b->rc.left == a->rc.left && b->rc.top < a->rc.top);
            if (!swap) break;
            Monitor t = *a; *a = *b; *b = t;
        }
    }
    for (int i = 0; i < s.n; i++) out[i].dpi &= 0x7FFFFFFFu;
    return s.n;
}

static void monitors_init(void)
{
    g.nmon = scan_monitors(g.mon);
    if (!g.nmon) {
        g.nmon = 1;
        ZeroMemory(&g.mon[0], sizeof g.mon[0]);
        g.mon[0].rc = (RECT){ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &g.mon[0].work, 0);
        g.mon[0].dpi = 96;
    }
    for (int i = 0; i < PHI_MAX_WS; i++) g.ws[i].mon = -1;
    for (int m = 0; m < g.nmon; m++) { g.mon[m].ws = m; g.ws[m].mon = m; }
    POINT p;
    GetCursorPos(&p);
    g.cur_mon = mon_from_point(p);
}

static void monitors_refresh(void)
{
    Monitor nm[PHI_MAX_MON];
    int nn = scan_monitors(nm);
    if (!nn) return;
    int map[PHI_MAX_MON];
    for (int i = 0; i < g.nmon; i++) {
        map[i] = -1;
        for (int j = 0; j < nn; j++) if (!lstrcmpW(g.mon[i].name, nm[j].name)) { map[i] = j; break; }
        if (map[i] >= 0) nm[map[i]].ws = g.mon[i].ws;
    }
    for (int k = 0; k < PHI_MAX_WS; k++) {
        int m = g.ws[k].mon;
        if (m < 0) continue;
        int nmk = m < g.nmon ? map[m] : -1;
        g.ws[k].mon = nmk >= 0 ? nmk : 0;
    }
    for (int j = 0; j < nn; j++) {
        if (nm[j].ws >= 0) continue;
        int pick = -1;
        for (int k = 0; k < PHI_MAX_WS && pick < 0; k++) {
            int shown = 0;
            for (int q = 0; q < nn; q++) if (nm[q].ws == k) shown = 1;
            if (!shown && g.ws[k].mon == j) pick = k;
        }
        for (int k = 0; k < PHI_MAX_WS && pick < 0; k++) {
            int shown = 0;
            for (int q = 0; q < nn; q++) if (nm[q].ws == k) shown = 1;
            if (!shown && ws_is_empty(k)) pick = k;
        }
        for (int k = 0; k < PHI_MAX_WS && pick < 0; k++) {
            int shown = 0;
            for (int q = 0; q < nn; q++) if (nm[q].ws == k) shown = 1;
            if (!shown) pick = k;
        }
        nm[j].ws = pick;
        g.ws[pick].mon = j;
    }
    int cur = (g.cur_mon < g.nmon && map[g.cur_mon] >= 0) ? map[g.cur_mon] : 0;
    memcpy(g.mon, nm, sizeof(Monitor) * (size_t)nn);
    g.nmon = nn;
    g.cur_mon = cur;
    for (int k = 0; k < g.nwins; k++) {
        Win *w = g.wins[k];
        if (ws_visible(w->ws)) win_show(w); else win_hide(w);
    }
    structural_change();
    apply_all();
    for (int k = 0; k < g.nwins; k++) {
        Win *w = g.wins[k];
        RECT r;
        if (!w->floating || w->hidden || w->minimized || !GetWindowRect(w->hwnd, &r)) continue;
        POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        if (!MonitorFromPoint(c, MONITOR_DEFAULTTONULL)) place_floating_on_monitor(w, g.ws[w->ws].mon);
    }
    save_hidden_list();
    tray_update();
    phi_log("monitors changed: %d monitor(s)", nn);
}

/* closest monitor in a direction from monitor m, or -1 */
static int mon_in_dir(int m, char d)
{
    RECT a = g.mon[m].rc;
    int best = -1;
    long bestd = 0x7fffffff;
    for (int i = 0; i < g.nmon; i++) {
        if (i == m) continue;
        RECT b = g.mon[i].rc;
        long dist;
        switch (d) {
        case 'l': if (b.right > a.left) continue; dist = a.left - b.right; break;
        case 'r': if (b.left < a.right) continue; dist = b.left - a.right; break;
        case 'u': if (b.bottom > a.top) continue; dist = a.top - b.bottom; break;
        case 'd': if (b.top < a.bottom) continue; dist = b.top - a.bottom; break;
        default: continue;
        }
        if (dist < bestd) { bestd = dist; best = i; }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* geometry queries                                                    */
/* ------------------------------------------------------------------ */

static RECT win_rect(Win *w)
{
    RECT r;
    if (w->node) {
        r.left = w->tile.x; r.top = w->tile.y;
        r.right = w->tile.x + w->tile.w; r.bottom = w->tile.y + w->tile.h;
    } else {
        GetWindowRect(w->hwnd, &r);
    }
    return r;
}

static Win *neighbor_of_rect(RECT a, Win *from, char d)
{
    Win *best = NULL;
    long long bestscore = 0x7fffffffffffffffLL;
    for (int k = 0; k < g.nwins; k++) {
        Win *w = g.wins[k];
        if (w == from || !w->node || w->hidden || !ws_visible(w->ws)) continue;
        RECT b = win_rect(w);
        long dist, overlap;
        switch (d) {
        case 'l': if (b.right > a.left + 4) continue; dist = a.left - b.right;
                  overlap = min(a.bottom, b.bottom) - max(a.top, b.top); break;
        case 'r': if (b.left < a.right - 4) continue; dist = b.left - a.right;
                  overlap = min(a.bottom, b.bottom) - max(a.top, b.top); break;
        case 'u': if (b.bottom > a.top + 4) continue; dist = a.top - b.bottom;
                  overlap = min(a.right, b.right) - max(a.left, b.left); break;
        case 'd': if (b.top < a.bottom - 4) continue; dist = b.top - a.bottom;
                  overlap = min(a.right, b.right) - max(a.left, b.left); break;
        default: return NULL;
        }
        if (dist < 0) dist = 0;
        long long score = overlap > 0 ? (long long)dist * 100000 - overlap
                                      : 1000000000000LL + (long long)dist * 100000 - overlap;
        if (score < bestscore) { bestscore = score; best = w; }
    }
    return best;
}

static Win *tiled_at(POINT p, Win *exclude)
{
    for (int k = 0; k < g.nwins; k++) {
        Win *w = g.wins[k];
        if (w == exclude || !w->node || w->hidden || !ws_visible(w->ws)) continue;
        RECT r = win_rect(w);
        if (PtInRect(&r, p)) return w;
    }
    return NULL;
}

/* Active window for dispatchers: the foreground window if we manage it,
 * otherwise (strict == 0) the remembered focus of the focused workspace. */
static Win *active_win(int strict)
{
    Win *w = win_find(GetForegroundWindow());
    if (w && ws_visible(w->ws)) return w;
    if (strict || !g.nmon) return NULL;
    return g.ws[cur_ws()].focus;
}

/* ------------------------------------------------------------------ */
/* dispatchers                                                         */
/* ------------------------------------------------------------------ */

typedef struct { wchar_t file[1024]; wchar_t args[1024]; int has_args; wchar_t dir[MAX_PATH]; char cmd8[512]; } ExecJob;

/* ShellExecuteEx can block for seconds (network paths, slow app paths) and pumps
 * COM messages, so it runs on its own short-lived thread. */
static DWORD WINAPI exec_thread(LPVOID p)
{
    ExecJob *j = p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = j->file;
    sei.lpParameters = j->has_args ? j->args : NULL;
    sei.lpDirectory = j->dir[0] ? j->dir : NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) phi_log("exec failed (%lu): %s", GetLastError(), j->cmd8);
    CoUninitialize();
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

static void run_command(const char *cmd8)
{
    wchar_t w[1024], exp[2048];
    wide_from_utf8(cmd8, w, 1024);
    if (!ExpandEnvironmentStringsW(w, exp, 2048)) lstrcpynW(exp, w, 2048);
    wchar_t *p = exp, *file;
    while (*p == L' ') p++;
    if (*p == L'"') {
        file = ++p;
        while (*p && *p != L'"') p++;
    } else {
        file = p;
        while (*p && *p != L' ') p++;
    }
    if (*p) *p++ = 0;
    while (*p == L' ') p++;
    if (!*file) return;

    ExecJob *j = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *j);
    if (!j) return;
    lstrcpynW(j->file, file, 1024);
    lstrcpynW(j->args, p, 1024);
    j->has_args = *p != 0;
    if (!GetEnvironmentVariableW(L"USERPROFILE", j->dir, MAX_PATH)) j->dir[0] = 0;
    lstrcpynA(j->cmd8, cmd8, sizeof j->cmd8);
    HANDLE t = CreateThread(NULL, 0, exec_thread, j, 0, NULL);
    if (t) CloseHandle(t);
    else HeapFree(GetProcessHeap(), 0, j);
}

static int closable(HWND h)
{
    if (!h || h == g.msgwnd || !IsWindowVisible(h)) return 0;
    if (win_find(h)) return 1;
    wchar_t cls[128];
    GetClassNameW(h, cls, 128);
    if (class_ignored(cls)) return 0;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    return !(ex & WS_EX_TOOLWINDOW);
}

static void cmd_movefocus(char d)
{
    Win *a = active_win(0);
    RECT r;
    if (a) r = win_rect(a);
    else {
        RECT m = g.mon[g.cur_mon].rc;
        POINT c = { (m.left + m.right) / 2, (m.top + m.bottom) / 2 };
        r = (RECT){ c.x, c.y, c.x + 1, c.y + 1 };
    }
    Win *n = neighbor_of_rect(r, a, d);
    if (n) { focus_win(n); return; }
    int m = mon_in_dir(a ? g.ws[a->ws].mon : g.cur_mon, d);
    if (m < 0) return;
    int ws = g.mon[m].ws;
    Win *f = g.ws[ws].focus ? g.ws[ws].focus : ws_mru(ws, NULL, 0);
    if (f) focus_win(f); else focus_desktop_on(m);
}

static void cmd_swapwindow(char d)
{
    Win *a = active_win(0);
    if (!a || !a->node) return;
    RECT r = win_rect(a);
    Win *b = neighbor_of_rect(r, a, d);
    if (b) {
        swap_wins(a, b);
        apply_all();
        focus_win(a);
        return;
    }
    int m = mon_in_dir(g.ws[a->ws].mon, d);
    if (m < 0) return;
    move_to_ws(a, g.mon[m].ws, MOVE_KEEP_FOCUS, NULL, NULL);
    apply_all();
}

static void cmd_togglefloating(void)
{
    Win *a = active_win(0);
    if (!a || a->minimized) return;
    Workspace *ws = &g.ws[a->ws];
    if (a->floating) {
        if (GetWindowRect(a->hwnd, &a->float_rect)) a->has_float_rect = 1;
        a->floating = 0;
        tile_insert(a, NULL, -1, NULL);
    } else {
        tile_remove(a);
        a->floating = 1;
        if (ws->fs == a) fs_clear(ws);
        RECT wa = g.mon[ws->mon >= 0 ? ws->mon : 0].work;
        int W = wa.right - wa.left, H = wa.bottom - wa.top;
        RECT r = a->float_rect;
        int fw = a->has_float_rect ? r.right - r.left : W * 6 / 10;
        int fh = a->has_float_rect ? r.bottom - r.top : H * 6 / 10;
        if (fw > W * 9 / 10) fw = W * 9 / 10;
        if (fh > H * 9 / 10) fh = H * 9 / 10;
        int x = wa.left + (W - fw) / 2, y = wa.top + (H - fh) / 2;
        SetWindowPos(a->hwnd, HWND_TOP, x, y, fw, fh, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    structural_change();
    apply_ws(a->ws);
}

static void cmd_fullscreen(int mode)
{
    Win *a = active_win(0);
    if (!a) return;
    Workspace *ws = &g.ws[a->ws];
    if (ws->fs == a) {
        fs_clear(ws);
        if (IsZoomed(a->hwnd)) ShowWindowAsync(a->hwnd, SW_SHOWNOACTIVATE);
        if (a->floating && a->has_float_rect) {
            RECT r = a->float_rect;
            SetWindowPos(a->hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    } else {
        if (a->floating && GetWindowRect(a->hwnd, &a->float_rect)) a->has_float_rect = 1;
        ws->fs = a;
        ws->fs_mode = mode == 1 ? 1 : 0;
    }
    structural_change();
    apply_ws(a->ws);
}

static void cmd_resizeactive(const char *arg)
{
    Win *a = active_win(0);
    if (!a) return;
    int dx = 0, dy = 0;
    if (sscanf(arg, "%d %d", &dx, &dy) < 1) return;
    UINT dpi = g.mon[g.ws[a->ws].mon >= 0 ? g.ws[a->ws].mon : 0].dpi;
    dx = scale(dx, dpi);
    dy = scale(dy, dpi);
    if (a->node) {
        phi_resize(a->node, dx, dy);
        structural_change();
        apply_ws(a->ws);
    } else if (a->floating) {
        RECT r;
        if (!GetWindowRect(a->hwnd, &r)) return;
        SetWindowPos(a->hwnd, NULL, r.left, r.top, (r.right - r.left) + dx, (r.bottom - r.top) + dy,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

static void cmd_splitratio(const char *arg)
{
    Win *a = active_win(0);
    if (!a || !a->node) return;
    while (*arg == ' ') arg++;
    if (!_strnicmp(arg, "exact", 5)) phi_split_ratio(a->node, atof(arg + 5) / 2.0, 1);
    else phi_split_ratio(a->node, atof(arg) / 2.0, 0);   /* hyprland scale: 1.0 = 50/50 */
    structural_change();
    apply_ws(a->ws);
}

static void cmd_cyclenext(const char *arg)
{
    int ws = cur_ws();
    Win *list[PHI_MAX_WIN];
    int n = 0;
    for (PhiNode *nd = phi_first(&g.ws[ws].tree); nd; nd = phi_next(nd)) list[n++] = nd->win;
    for (int k = 0; k < g.nwins; k++)
        if (g.wins[k]->ws == ws && g.wins[k]->floating && !g.wins[k]->minimized) list[n++] = g.wins[k];
    if (!n) return;
    Win *a = active_win(0);
    int pos = -1;
    for (int i = 0; i < n; i++) if (list[i] == a) pos = i;
    int step = (arg && !_strnicmp(arg, "prev", 4)) ? -1 : 1;
    int k = pos < 0 ? 0 : ((pos + step) % n + n) % n;
    focus_win(list[k]);
}

static void cmd_focusmonitor(const char *arg)
{
    int m = -1;
    while (*arg == ' ') arg++;
    if (strchr("lrud", arg[0]) && arg[0] && !arg[1]) m = mon_in_dir(g.cur_mon, arg[0]);
    else if (arg[0] == '+' || arg[0] == '-') m = ((g.cur_mon + atoi(arg)) % g.nmon + g.nmon) % g.nmon;
    else if (arg[0] >= '0' && arg[0] <= '9') m = atoi(arg);
    if (m < 0 || m >= g.nmon) return;
    int ws = g.mon[m].ws;
    Win *f = g.ws[ws].focus ? g.ws[ws].focus : ws_mru(ws, NULL, 0);
    if (f) focus_win(f); else focus_desktop_on(m);
}

static void dispatch(const char *disp, const char *arg)
{
    if (!arg) arg = "";
    if (!strcmp(disp, "exec")) { run_command(arg); return; }
    if (!strcmp(disp, "exit")) { wm_quit(); return; }
    if (!strcmp(disp, "refresh")) { wm_refresh(); return; }
    if (!strcmp(disp, "killactive")) {
        HWND fg = GetForegroundWindow();
        if (closable(fg)) PostMessageW(fg, WM_CLOSE, 0, 0);
        return;
    }
    if (!strcmp(disp, "movefocus")) { cmd_movefocus(arg[0]); return; }
    if (!strcmp(disp, "movewindow") || !strcmp(disp, "swapwindow")) {
        char d = arg[0];
        if (!_strnicmp(arg, "mon:", 4)) d = arg[4];
        cmd_swapwindow(d);
        return;
    }
    if (!strcmp(disp, "togglesplit") || (!strcmp(disp, "layoutmsg") && !_stricmp(arg, "togglesplit"))) {
        Win *a = active_win(0);
        if (a && a->node) { phi_toggle_split(a->node); structural_change(); apply_ws(a->ws); }
        return;
    }
    if (!strcmp(disp, "layoutmsg")) { phi_log("layoutmsg %s is not supported", arg); return; }
    if (!strcmp(disp, "togglefloating")) { cmd_togglefloating(); return; }
    if (!strcmp(disp, "pseudo")) {
        Win *a = active_win(0);
        if (!a) return;
        a->pseudo = !a->pseudo;
        structural_change();
        apply_ws(a->ws);
        return;
    }
    if (!strcmp(disp, "fullscreen")) { cmd_fullscreen(atoi(arg)); return; }
    if (!strcmp(disp, "workspace")) {
        int n = resolve_ws(arg);
        if (n >= 0) switch_ws(n, NULL);
        return;
    }
    if (!strcmp(disp, "movetoworkspace") || !strcmp(disp, "movetoworkspacesilent")) {
        Win *a = active_win(0);
        int n = resolve_ws(arg);
        if (a && n >= 0) move_to_ws(a, n, strcmp(disp, "movetoworkspace") ? MOVE_SILENT : MOVE_FOLLOW, NULL, NULL);
        return;
    }
    if (!strcmp(disp, "focusmonitor")) { cmd_focusmonitor(arg); return; }
    if (!strcmp(disp, "splitratio")) { cmd_splitratio(arg); return; }
    if (!strcmp(disp, "resizeactive")) { cmd_resizeactive(arg); return; }
    if (!strcmp(disp, "cyclenext")) { cmd_cyclenext(arg); return; }
    phi_log("unknown dispatcher: %s", disp);
}

/* ------------------------------------------------------------------ */
/* WinEvent handling                                                   */
/* ------------------------------------------------------------------ */

static int is_desktop(HWND h)
{
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return 0;
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW");
}

static void on_foreground(HWND h)
{
    if (g.pending_focus && h == g.pending_focus) { KillTimer(g.msgwnd, T_FOCUS); g.pending_focus = NULL; }
    Win *w = win_find(h);
    if (!w) w = manage(h, 0);   /* the foreground event can beat the show event: take it over now */
    if (!w) {
        mark_active(NULL);
        if (is_desktop(h)) {
            if (now() - desktop_focus_tick > 500) {
                POINT p;
                GetCursorPos(&p);
                g.cur_mon = mon_from_point(p);
            }
        } else if (IsWindowVisible(h)) {
            g.cur_mon = mon_from_hwnd(h);
        }
        tray_update();
        return;
    }
    if (!ws_visible(w->ws)) {
        if (w->hidden || !IsWindowVisible(h) || now() - w->vis_tick < 500) return;
        switch_ws(w->ws, w);
        return;
    }
    Workspace *ws = &g.ws[w->ws];
    if (ws->fs && ws->fs != w && w->node && ws->fs_mode != 2) {
        fs_clear(ws);
        structural_change();
        apply_ws(w->ws);
    }
    ws->focus = w;
    w->focus_stamp = ++g.stamp;
    g.cur_mon = ws->mon;
    mark_active(w);
    tray_update();
}

/* Focus follows the mouse, polled every 40 ms (a second low-level hook would only
 * add input latency). The focus moves to the managed window under the cursor when
 * the cursor really moved, no button is held, nothing is being dragged, no menu is
 * open, and the current focus is a managed window or the desktop. So menus, popups,
 * dialogs and Start keep theirs, and the desktop between the gaps changes nothing. */
static void mouse_tick(void)
{
    POINT p;
    if (!GetCursorPos(&p) || (p.x == mouse_last.x && p.y == mouse_last.y)) return;
    mouse_last = p;
    if (g.dragging || g.pending_focus) return;
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON) |
         GetAsyncKeyState(VK_XBUTTON1) | GetAsyncKeyState(VK_XBUTTON2)) & 0x8000) return;
    GUITHREADINFO gti;
    gti.cbSize = sizeof gti;
    if (GetGUIThreadInfo(0, &gti) &&
        (gti.hwndCapture || (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE))))
        return;
    HWND h = WindowFromPoint(p);
    if (!h) return;
    Win *w = win_find(GetAncestor(h, GA_ROOT));
    if (!w || w->hidden || w->minimized || !ws_visible(w->ws)) return;
    HWND fg = GetForegroundWindow();
    if (fg == w->hwnd) return;
    if (fg && !win_find(fg) && !is_desktop(fg)) return;
    phi_log("hover: focus %s", w->exe);
    focus_win_ex(w, 0);
}

static void on_location(Win *w)
{
    if (g.dragging == w || w->hidden || w->minimized || !ws_visible(w->ws)) return;
    Workspace *ws = &g.ws[w->ws];
    HWND h = w->hwnd;
    int zoomed = IsZoomed(h);
    if (zoomed && ws->fs != w) {
        /* the app or the user maximized it: let it be, like fullscreen 1 */
        ws->fs = w;
        ws->fs_mode = 2;
        return;
    }
    if (!zoomed && ws->fs == w && ws->fs_mode == 2) {
        fs_clear(ws);
        structural_change();
        SetTimer(g.msgwnd, T_RETILE, 60, NULL);
        return;
    }
    if (ws->fs == w || w->floating || !w->has_placed) return;
    RECT r;
    if (!GetWindowRect(h, &r) || EqualRect(&r, &w->placed)) return;
    /* something else moved a tiled window: put it back, but don't fight forever */
    if (w->fix_epoch != g.epoch) { w->fix_epoch = g.epoch; w->fix_count = 0; }
    if (w->fix_count < 4) {
        w->fix_count++;
        SetTimer(g.msgwnd, T_RETILE, 80, NULL);
        return;
    }
    if (w->fix_count > 4) return;
    /* it insists: it has a minimum size we did not know, or one that cannot fit */
    w->fix_count++;
    int mw = w->min_w, mh = w->min_h;
    read_min_size(w);
    phi_log("%p keeps %ldx%ld instead of %ldx%ld (minimum %dx%d)", (void *)h,
            (long)(r.right - r.left), (long)(r.bottom - r.top),
            (long)(w->placed.right - w->placed.left), (long)(w->placed.bottom - w->placed.top), w->min_w, w->min_h);
    if (mw != w->min_w || mh != w->min_h) SetTimer(g.msgwnd, T_RETILE, 80, NULL);
}

static void on_drag_end(Win *w)
{
    g.dragging = NULL;
    if (w->minimized || w->hidden) return;
    RECT r;
    if (!GetWindowRect(w->hwnd, &r)) return;
    if (w->floating) {
        w->float_rect = r;
        w->has_float_rect = 1;
        /* a floating window dragged to another monitor joins that monitor's workspace */
        POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        int mi = mon_from_point(c);
        if (ws_visible(w->ws) && mi != g.ws[w->ws].mon) {
            Workspace *s = &g.ws[w->ws];
            if (s->focus == w) s->focus = ws_mru(w->ws, w, 0);
            if (s->fs == w) fs_clear(s);
            w->ws = g.mon[mi].ws;
            g.ws[w->ws].focus = w;
            g.cur_mon = mi;
            tray_update();
        }
        return;
    }
    if (!w->node || !ws_visible(w->ws)) return;

    RECT m = w->has_margin ? w->margin : (RECT){ 0, 0, 0, 0 };
    PhiRect v = { r.left + m.left, r.top + m.top,
                  (r.right - m.right) - (r.left + m.left), (r.bottom - m.bottom) - (r.top + m.top) };
    PhiRect t = w->tile;
    int dl = v.x - t.x, dr = (v.x + v.w) - (t.x + t.w);
    int dt = v.y - t.y, db = (v.y + v.h) - (t.y + t.h);
    int resized = abs(v.w - t.w) > 3 || abs(v.h - t.h) > 3;
    int gin;
    ws_area(w->ws, &gin);

    if (resized) {
        if (abs(dl) > 3) phi_move_edge(w->node, PHI_EDGE_LEFT, v.x - gin);
        if (abs(dr) > 3) phi_move_edge(w->node, PHI_EDGE_RIGHT, v.x + v.w + gin);
        if (abs(dt) > 3) phi_move_edge(w->node, PHI_EDGE_TOP, v.y - gin);
        if (abs(db) > 3) phi_move_edge(w->node, PHI_EDGE_BOTTOM, v.y + v.h + gin);
    } else if (abs(dl) > 3 || abs(dt) > 3) {
        POINT p;
        GetCursorPos(&p);
        Win *target = tiled_at(p, w);
        if (target && target->ws == w->ws) swap_wins(w, target);
        else if (target) move_to_ws(w, target->ws, MOVE_KEEP_FOCUS, target, &p);
        else {
            int mi = mon_from_point(p);
            if (mi != g.ws[w->ws].mon) move_to_ws(w, g.mon[mi].ws, MOVE_KEEP_FOCUS, NULL, NULL);
        }
    }
    structural_change();
    apply_all();
    if (GetForegroundWindow() == w->hwnd) mark_active(w);
}

static void CALLBACK on_event(HWINEVENTHOOK hook, DWORD ev, HWND h, LONG obj, LONG child,
                              DWORD tid, DWORD time)
{
    (void)hook; (void)tid; (void)time;
    if (!h || obj != OBJID_WINDOW || child != CHILDID_SELF || g.quitting) return;
    Win *w = win_find(h);
    switch (ev) {
    case EVENT_OBJECT_SHOW:
        if (w) {
            /* A window phiwm hid became visible. If it also took focus (you clicked its
             * tray icon, it raised itself), follow it to its workspace. Otherwise it is a
             * busy app processing an old show: hide it again. It is never unmanaged
             * here, so it can't get lost. */
            if (w->hidden && IsWindowVisible(h)) {
                if (GetForegroundWindow() == h && now() - w->vis_tick > 500) {
                    switch_ws(w->ws, w);
                } else {
                    w->vis_tick = now();
                    ShowWindowAsync(h, SW_HIDE);
                }
            }
            return;
        }
        manage(h, 0);
        break;
    case EVENT_OBJECT_UNCLOAKED:
    case EVENT_OBJECT_NAMECHANGE:
        if (!w) manage(h, 0);
        break;
    case EVENT_OBJECT_HIDE:
        if (w && !w->hidden && now() - w->vis_tick > 500 && !IsWindowVisible(h)) unmanage(w, 1);
        break;
    case EVENT_OBJECT_CLOAKED:
        if (w && !w->hidden && is_cloaked(h)) unmanage(w, 1);
        break;
    case EVENT_OBJECT_DESTROY:
        if (w) unmanage(w, 0);
        break;
    case EVENT_SYSTEM_FOREGROUND:
        on_foreground(h);
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
        if (w && !w->minimized) {
            w->minimized = 1;
            if (g.ws[w->ws].fs == w) fs_clear(&g.ws[w->ws]);
            tile_remove(w);
            structural_change();
            apply_ws(w->ws);
        }
        break;
    case EVENT_SYSTEM_MINIMIZEEND:
        if (w && w->minimized) {
            w->minimized = 0;
            if (!w->floating) tile_insert(w, NULL, -1, NULL);
            structural_change();
            apply_ws(w->ws);
            SetTimer(g.msgwnd, T_SETTLE1, 150, NULL);   /* it may still be iconic right now */
        }
        break;
    case EVENT_SYSTEM_MOVESIZESTART:
        if (w) g.dragging = w;
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        if (w) on_drag_end(w);
        else g.dragging = NULL;
        break;
    case EVENT_OBJECT_LOCATIONCHANGE:
        if (w) on_location(w);
        break;
    }
}

static void hooks_install(void)
{
    static const DWORD ranges[][2] = {
        { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
        { EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND },
        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
        { EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE },
        { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE },
        { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
    };
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        HWINEVENTHOOK h = SetWinEventHook(ranges[i][0], ranges[i][1], NULL, on_event, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) g.hooks[g.nhooks++] = h;
        else phi_log("SetWinEventHook failed for 0x%lx", (unsigned long)ranges[i][0]);
    }
}

/* ------------------------------------------------------------------ */
/* startup scan                                                        */
/* ------------------------------------------------------------------ */

typedef struct { HWND list[PHI_MAX_WIN]; int n; } Scan;

static BOOL CALLBACK scan_cb(HWND h, LPARAM lp)
{
    Scan *s = (Scan *)lp;
    if (s->n < PHI_MAX_WIN) s->list[s->n++] = h;
    return TRUE;
}

static void manage_existing(void)
{
    static Scan s;
    s.n = 0;
    EnumWindows(scan_cb, (LPARAM)&s);
    /* EnumWindows is top of the z-order first; insert bottom-up so the top window ends up focused */
    for (int i = s.n - 1; i >= 0; i--) manage(s.list[i], 1);
    for (int m = 0; m < g.nmon; m++) g.ws[g.mon[m].ws].focus = ws_mru(g.mon[m].ws, NULL, 0);
    apply_all();
    Win *fg = win_find(GetForegroundWindow());
    if (fg) { g.ws[fg->ws].focus = fg; g.cur_mon = g.ws[fg->ws].mon >= 0 ? g.ws[fg->ws].mon : g.cur_mon; mark_active(fg); }
    phi_log("managing %d existing window(s)", g.nwins);
}

/* ------------------------------------------------------------------ */
/* refresh, log                                                        */
/* ------------------------------------------------------------------ */

/* Win+Ctrl+R: rescan monitors, retile everything, reinstall the keyboard hook.
 * The way out when something external left the layout or the hook in a bad state. */
void wm_refresh(void)
{
    phi_log("refresh");
    keys_rehook();
    monitors_refresh();
}

void wm_open_log(void)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\phiwm.log", g.data_dir);
    path[MAX_PATH - 1] = 0;
    if ((INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
}

void wm_quit(void)
{
    PostQuitMessage(0);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void cleanup(void)
{
    if (g.quitting) return;
    g.quitting = 1;
    for (int i = 0; i < g.nhooks; i++) UnhookWinEvent(g.hooks[i]);
    g.nhooks = 0;
    keys_stop();
    for (int i = 0; i < g.nwins; i++) {
        Win *w = g.wins[i];
        if (w->hidden) show_owned(w);
        if (!IsWindow(w->hwnd)) continue;
        if (w->fs_styled) fs_style(w, 0);
        if (w->hidden) ShowWindowAsync(w->hwnd, SW_SHOWNA);
        COLORREF c = COLOR_DEFAULT;
        DwmSetWindowAttribute(w->hwnd, A_BORDER_COLOR, &c, sizeof c);
        set_corners(w, 1);
        set_transitions(w->hwnd, 0);
    }
    tray_remove();
    wchar_t path[MAX_PATH];
    hidden_list_path(path);
    DeleteFileW(path);
    phi_log("bye");
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *e)
{
    phi_log("crash: exception 0x%08lx at %p", (unsigned long)e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress);
    for (int i = 0; i < g.nwins; i++) {
        Win *w = g.wins[i];
        set_transitions(w->hwnd, 0);
        if (w->fs_styled) fs_style(w, 0);
        if (!w->hidden) continue;
        ShowWindowAsync(w->hwnd, SW_SHOWNA);
        for (int k = 0; k < w->nowned && k < 16; k++) ShowWindowAsync(w->owned[k], SW_SHOWNA);
    }
    tray_remove();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void handle_ipc(const char *msg)
{
    char buf[512];
    lstrcpynA(buf, msg, sizeof buf);
    char *disp = buf;
    while (*disp == ' ') disp++;
    char *arg = disp;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;
    if (!phi_dispatcher_known(disp)) { phi_log("ipc: unknown dispatcher %s", disp); return; }
    phi_log("ipc: %s %s", disp, arg);
    dispatch(disp, arg);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PHI_BIND:
        if ((int)wp >= 0 && (int)wp < phi_nbinds) {
            const PhiBind *b = &phi_binds[wp];
            phi_log("bind: %s %s", b->disp, b->arg);
            dispatch(b->disp, b->arg);
        }
        return 0;
    case WM_PHI_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU: case NIN_SELECT: case NIN_KEYSELECT:
            tray_menu();
            break;
        }
        return 0;
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (cd && cd->dwData == COPYDATA_MAGIC && cd->cbData > 0 && cd->cbData < 512) {
            char buf[512];
            memcpy(buf, cd->lpData, cd->cbData);
            buf[cd->cbData] = 0;
            handle_ipc(buf);
            return TRUE;
        }
        return FALSE;
    }
    case WM_TIMER:
        switch (wp) {
        case T_RETILE: case T_SETTLE1: case T_SETTLE2:
            KillTimer(h, wp);
            apply_all();
            break;
        case T_FOCUS:
            if (!g.pending_focus || GetForegroundWindow() == g.pending_focus || ++g.focus_tries > 4 ||
                !IsWindow(g.pending_focus)) {
                KillTimer(h, T_FOCUS);
                g.pending_focus = NULL;
            } else {
                Win *w = win_find(g.pending_focus);
                force_foreground(g.pending_focus);
                if (w) mark_active(w);
            }
            break;
        case T_REHOOK:
            keys_rehook();
            break;
        case T_MOUSE:
            mouse_tick();
            break;
        case T_MONITORS:
            KillTimer(h, T_MONITORS);
            monitors_refresh();
            break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        SetTimer(h, T_MONITORS, 400, NULL);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) SetTimer(h, T_MONITORS, 400, NULL);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            keys_rehook();
            SetTimer(h, T_MONITORS, 1500, NULL);
        }
        return TRUE;
    case WM_ENDSESSION:
        if (wp) cleanup();
        return 0;
    }
    if (msg == tray_taskbar_created_msg() && msg) { tray_add(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* command line client                                                 */
/* ------------------------------------------------------------------ */

static void console_print(const char *s)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, s, (DWORD)strlen(s), &n, NULL);
}

static int run_client(int argc, wchar_t **argv)
{
    AttachConsole(ATTACH_PARENT_PROCESS);
    char cmd[512] = "";
    if (!lstrcmpW(argv[1], L"--version") || !lstrcmpW(argv[1], L"-v")) {
        console_print("phiwm " PHI_VERSION_STR "\n");
        return 0;
    }
    if (!lstrcmpW(argv[1], L"--help") || !lstrcmpW(argv[1], L"-h") || !lstrcmpW(argv[1], L"/?")) {
        console_print("phiwm " PHI_VERSION_STR " - dwindle tiling for Windows\n\n"
                      "  phiwm                     start the window manager\n"
                      "  phiwm dispatch <d> [arg]  run a dispatcher, e.g. phiwm dispatch workspace 3\n"
                      "  phiwm quit                quit and show all windows\n");
        return 0;
    }
    int start = 1;
    if (!lstrcmpW(argv[1], L"dispatch")) start = 2;
    else if (!lstrcmpW(argv[1], L"quit")) { lstrcpyA(cmd, "exit"); start = argc; }
    for (int i = start; i < argc; i++) {
        char part[256];
        utf8_from_wide(argv[i], part, sizeof part);
        if (cmd[0]) lstrcatA(cmd, " ");
        if (lstrlenA(cmd) + lstrlenA(part) < (int)sizeof cmd - 1) lstrcatA(cmd, part);
    }
    HWND target = FindWindowW(PHI_CLASS_MSG, NULL);
    if (!target) { console_print("phiwm is not running\n"); return 1; }
    COPYDATASTRUCT cd = { COPYDATA_MAGIC, (DWORD)strlen(cmd), cmd };
    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) || !res) {
        console_print("phiwm did not accept the command\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

static void init_paths(void)
{
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) lstrcpyW(local, L"C:\\");
    _snwprintf(g.data_dir, MAX_PATH, L"%ls\\phiwm", local);
    g.data_dir[MAX_PATH - 1] = 0;
    ensure_dir(g.data_dir);
}

static int self_is_elevated(void)
{
    HANDLE tok;
    int e = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te;
        DWORD n;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof te, &n)) e = te.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return e;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) return run_client(argc, argv);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\phiwm.instance");
    DWORD merr = GetLastError();
    if (!mutex || merr == ERROR_ALREADY_EXISTS || merr == ERROR_ACCESS_DENIED) return 0;

    g.inst = inst;
    init_paths();
    log_open();
    phi_log("phiwm %s starting", PHI_VERSION_STR);
    SetUnhandledExceptionFilter(crash_handler);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g.self_elevated = self_is_elevated();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = PHI_CLASS_MSG;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    g.msgwnd = CreateWindowExW(WS_EX_TOOLWINDOW, PHI_CLASS_MSG, L"phiwm", WS_POPUP, 0, 0, 0, 0,
                               NULL, NULL, inst, NULL);
    if (!g.msgwnd) { phi_log("cannot create window"); return 1; }
    ChangeWindowMessageFilterEx(g.msgwnd, tray_taskbar_created_msg(), MSGFLT_ALLOW, NULL);

    monitors_init();

    /* hooks first: windows that appear while we scan are picked up once the loop runs
     * (manage() ignores windows it already has) */
    hooks_install();
    restore_hidden_from_file();
    manage_existing();
    save_hidden_list();
    if (!keys_start(g.msgwnd)) phi_log("keyboard hook failed (%lu)", GetLastError());
    tray_add();
    SetTimer(g.msgwnd, T_REHOOK, 5 * 60 * 1000, NULL);
    if (PHI_FOLLOW_MOUSE) { GetCursorPos(&mouse_last); SetTimer(g.msgwnd, T_MOUSE, 40, NULL); }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    cleanup();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    return 0;
}
