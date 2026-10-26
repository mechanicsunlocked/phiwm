/* phiwm — global keybinds through a low-level keyboard hook.
 *
 * The hook lives on its own high-priority thread and does nothing but match
 * keys and post the bind index to the main thread, so a slow or hung window
 * can never make the keyboard lag. Matched keys are swallowed (key down and
 * the matching key up). When a bind uses Win or Alt, an unassigned key
 * (vk 0xE8) is injected so releasing Win does not open Start and releasing
 * Alt does not open an app's menu bar.
 */
#include "phiwm.h"

typedef struct { unsigned mods, vk, scan; int repeat; } HookBind;

static struct {
    SRWLOCK  lock;
    HookBind binds[PHI_MAX_BINDS];
    int      n;
    unsigned gen;
    HWND     target;
    HANDLE   thread;
    DWORD    tid;
    HHOOK    hook;
    unsigned char down[256];   /* keys swallowed on key down, still held */
    unsigned char passed[256]; /* keys whose key down went to the app, still held */
} K;

static int is_modifier(DWORD vk)
{
    switch (vk) {
    case VK_LWIN: case VK_RWIN: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
        return 1;
    }
    return 0;
}

static int held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* Right Alt never counts as Alt: on European layouts it is AltGr, which Windows
 * reports as Ctrl+Alt (with a fake left Ctrl), and typing @ { } € must never fire
 * a CTRL ALT bind. */
static unsigned current_mods(void)
{
    unsigned m = 0;
    int altgr = held(VK_RMENU);
    if (held(VK_LWIN) || held(VK_RWIN)) m |= PHI_MOD_SUPER;
    if (held(VK_SHIFT)) m |= PHI_MOD_SHIFT;
    if (altgr ? held(VK_RCONTROL) : held(VK_CONTROL)) m |= PHI_MOD_CTRL;
    if (held(VK_LMENU)) m |= PHI_MOD_ALT;
    return m;
}

static void send_mask_key(void)
{
    INPUT in[2];
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = 0xE8;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof in[0]);
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code != HC_ACTION) return CallNextHookEx(NULL, code, wp, lp);
    const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(NULL, code, wp, lp);

    DWORD vk = k->vkCode & 0xFF;
    int down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    if (is_modifier(vk)) return CallNextHookEx(NULL, code, wp, lp);

    if (!down) {
        K.passed[vk] = 0;
        if (K.down[vk]) { K.down[vk] = 0; return 1; }
        return CallNextHookEx(NULL, code, wp, lp);
    }
    /* Autorepeat of a key that was already going to the app (holding W in a game,
     * then tapping Win) must keep going to the app: never turn it into a bind. */
    if (K.passed[vk]) return CallNextHookEx(NULL, code, wp, lp);

    unsigned mods = current_mods();
    int idx = -1, repeat = 0;
    unsigned gen;
    AcquireSRWLockShared(&K.lock);
    for (int i = 0; i < K.n; i++) {
        const HookBind *b = &K.binds[i];
        if (b->mods != mods) continue;
        int hit = b->scan ? (k->scanCode == b->scan && !(k->flags & LLKHF_EXTENDED)) : (b->vk == vk);
        if (hit) { idx = i; repeat = b->repeat; break; }
    }
    gen = K.gen;
    ReleaseSRWLockShared(&K.lock);

    if (idx < 0) {
        K.down[vk] = 0;   /* a fresh press means any earlier key up was missed */
        K.passed[vk] = 1;
        return CallNextHookEx(NULL, code, wp, lp);
    }
    int was_down = K.down[vk];
    K.down[vk] = 1;
    if (mods & (PHI_MOD_SUPER | PHI_MOD_ALT)) send_mask_key();
    if (!was_down || repeat) PostMessageW(K.target, WM_PHI_BIND, (WPARAM)idx, (LPARAM)gen);
    return 1;
}

static DWORD WINAPI hook_thread(LPVOID ready)
{
    MSG m;
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);   /* make sure the thread has a queue */
    K.hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_proc, GetModuleHandleW(NULL), 0);
    SetEvent((HANDLE)ready);
    if (!K.hook) return 1;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_PHI_REHOOK) {
            /* Windows silently drops low-level hooks that time out; re-installing is cheap. */
            HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, ll_proc, GetModuleHandleW(NULL), 0);
            if (h) { UnhookWindowsHookEx(K.hook); K.hook = h; }
            ZeroMemory(K.down, sizeof K.down);
            ZeroMemory(K.passed, sizeof K.passed);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnhookWindowsHookEx(K.hook);
    K.hook = NULL;
    return 0;
}

int keys_start(HWND target)
{
    InitializeSRWLock(&K.lock);
    K.target = target;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    K.thread = CreateThread(NULL, 0, hook_thread, ready, 0, &K.tid);
    if (!K.thread) { CloseHandle(ready); return 0; }
    SetThreadPriority(K.thread, THREAD_PRIORITY_TIME_CRITICAL);
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    return K.hook != NULL;
}

void keys_rehook(void)
{
    if (K.tid) PostThreadMessageW(K.tid, WM_PHI_REHOOK, 0, 0);
}

void keys_stop(void)
{
    if (!K.thread) return;
    PostThreadMessageW(K.tid, WM_QUIT, 0, 0);
    WaitForSingleObject(K.thread, 2000);
    CloseHandle(K.thread);
    K.thread = NULL;
    K.tid = 0;
}

void keys_set_binds(const PhiConfig *c, unsigned gen)
{
    AcquireSRWLockExclusive(&K.lock);
    K.n = 0;
    for (int i = 0; i < c->nbinds && i < PHI_MAX_BINDS; i++) {
        K.binds[i].mods = c->binds[i].mods;
        K.binds[i].vk = c->binds[i].vk;
        K.binds[i].scan = c->binds[i].scan;
        K.binds[i].repeat = c->binds[i].repeat;
        K.n++;
    }
    K.gen = gen;
    ReleaseSRWLockExclusive(&K.lock);
}
