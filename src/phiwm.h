/* phiwm — shared declarations for the Windows side */
#ifndef PHIWM_H
#define PHIWM_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include "layout.h"
#include "config.h"

#define PHI_VERSION_STR "0.1.0"
#define PHI_MAX_WS      10
#define PHI_MAX_MON     8
#define PHI_MAX_WIN     512

/* window messages */
#define WM_PHI_BIND     (WM_APP + 1)   /* wParam = bind index, lParam = config generation */
#define WM_PHI_TRAY     (WM_APP + 2)
#define WM_PHI_REHOOK   (WM_APP + 3)

#define PHI_CLASS_MSG   L"phiwm.main"

typedef struct Win {
    HWND     hwnd;
    int      ws;            /* workspace index 0..PHI_MAX_WS-1 */
    PhiNode *node;          /* leaf in the workspace tree, NULL when floating/minimized */
    int      floating;
    int      pseudo;
    int      minimized;
    int      hidden;        /* hidden by phiwm (workspace not shown) */
    HWND     owned[16];     /* dialogs/palettes it owned, hidden along with it */
    int      nowned;
    ULONGLONG vis_tick;     /* when phiwm last changed its visibility */
    RECT     float_rect;    /* outer rect used when floating */
    int      has_float_rect;
    RECT     margin;        /* invisible frame: left/top/right/bottom, in px */
    int      has_margin;
    PhiRect  tile;          /* last visual rect phiwm gave it */
    RECT     placed;        /* last outer rect phiwm asked for */
    int      has_placed;
    unsigned focus_stamp;
    int      fix_count;     /* layout corrections since the last structural change */
    unsigned fix_epoch;
    int      border_state;  /* 0 none, 1 inactive, 2 active */
    wchar_t  cls[128];
    char     exe[MAX_PATH];
} Win;

typedef struct {
    PhiTree  tree;
    int      mon;           /* monitor index, -1 when not bound */
    Win     *focus;         /* last focused window */
    Win     *fs;            /* fullscreen window */
    int      fs_mode;       /* 0 fullscreen, 1 maximize (keeps gaps), 2 maximized by the app */
} Workspace;

typedef struct {
    HMONITOR h;
    RECT     rc, work;
    UINT     dpi;
    int      ws;            /* active workspace index */
    wchar_t  name[32];
} Monitor;

typedef struct {
    HINSTANCE inst;
    HWND      msgwnd;
    PhiConfig *cfg;
    unsigned  cfg_gen;
    wchar_t   cfg_path[MAX_PATH];
    wchar_t   data_dir[MAX_PATH];
    FILETIME  cfg_time;

    Win      *wins[PHI_MAX_WIN];
    int       nwins;
    Workspace ws[PHI_MAX_WS];
    Monitor   mon[PHI_MAX_MON];
    int       nmon;
    int       cur_mon;
    int       prev_ws;
    unsigned  stamp;
    unsigned  epoch;
    Win      *dragging;
    Win      *active_border_win;
    HWND      pending_focus;
    int       focus_tries;
    int       self_elevated;
    int       quitting;
    int       follow_mouse_set;
    BOOL      saved_xmouse, saved_xmouse_z;
    DWORD     saved_xmouse_delay;
    HWINEVENTHOOK hooks[8];
    int       nhooks;
} Globals;

extern Globals g;

/* util.c */
void  phi_log(const char *fmt, ...);
void  log_open(void);
void  utf8_from_wide(const wchar_t *w, char *out, int n);
void  wide_from_utf8(const char *s, wchar_t *out, int n);
char *read_file_utf8(const wchar_t *path);
int   ensure_dir(const wchar_t *path);

/* keys.c */
int   keys_start(HWND target);
void  keys_stop(void);
void  keys_set_binds(const PhiConfig *c, unsigned gen);
void  keys_rehook(void);

/* tray.c */
void  tray_add(void);
void  tray_remove(void);
void  tray_update(void);
void  tray_balloon(const wchar_t *title, const wchar_t *text);
void  tray_menu(void);
UINT  tray_taskbar_created_msg(void);
int   autostart_get(void);
void  autostart_set(int on);

/* wm.c, used by tray.c */
void  wm_reload(int user);
void  wm_quit(void);
void  wm_open_config(int folder);
int   wm_focused_ws_number(void);

#endif
