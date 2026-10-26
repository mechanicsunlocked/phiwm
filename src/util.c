/* phiwm — logging, paths and string helpers */
#include "phiwm.h"
#include <stdio.h>
#include <stdarg.h>

static FILE *logf_;

void log_open(void)
{
    wchar_t path[MAX_PATH];
    if (!g.data_dir[0]) return;
    wchar_t old[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\phiwm.log", g.data_dir);
    path[MAX_PATH - 1] = 0;
    _snwprintf(old, MAX_PATH, L"%ls\\phiwm.old.log", g.data_dir);
    old[MAX_PATH - 1] = 0;
    MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);   /* keep the previous run (and its crash line) */
    logf_ = _wfopen(path, L"w");
}

void phi_log(const char *fmt, ...)
{
    if (!logf_) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(logf_, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    fputc('\n', logf_);
    fflush(logf_);
}

/* Converts to UTF-8; if the buffer is too small the text is cut (on a character
 * boundary) instead of coming back empty. */
void utf8_from_wide(const wchar_t *w, char *out, int n)
{
    if (n <= 0) return;
    int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    if (r > 0) { out[n - 1] = 0; return; }
    int wl = lstrlenW(w);
    int max = (n - 1) / 3;                 /* every UTF-16 unit is at most 3 UTF-8 bytes */
    if (wl > max) wl = max;
    if (wl > 0 && w[wl - 1] >= 0xD800 && w[wl - 1] <= 0xDBFF) wl--;   /* don't split a surrogate pair */
    r = wl > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, wl, out, n - 1, NULL, NULL) : 0;
    out[r > 0 ? r : 0] = 0;
}

void wide_from_utf8(const char *s, wchar_t *out, int n)
{
    if (n <= 0) return;
    int r = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    if (r <= 0) out[0] = 0;
    out[n - 1] = 0;
}

/* Creates every missing directory along `path`. */
int ensure_dir(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    lstrcpynW(tmp, path, MAX_PATH);
    for (wchar_t *p = tmp + 3; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
