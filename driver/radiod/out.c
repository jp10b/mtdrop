/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See out.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "out.h"

static HANDLE g_live = INVALID_HANDLE_VALUE;
static int g_at_line_start = 1;

int out_open_live(const char *path) {
    out_close_live();
    g_live = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    g_at_line_start = 1;
    return g_live == INVALID_HANDLE_VALUE ? -1 : 0;
}

void out_close_live(void) {
    if (g_live != INVALID_HANDLE_VALUE) CloseHandle(g_live);
    g_live = INVALID_HANDLE_VALUE;
}

/* One WriteFile per call: each line goes to disk as a whole, prefixed with the local time. */
static void live_write(const char *text, size_t len) {
    char buf[4200];
    size_t n = 0, i;
    if (g_live == INVALID_HANDLE_VALUE) return;
    for (i = 0; i < len && n + 32 < sizeof(buf); i++) {
        if (g_at_line_start) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%02u:%02u:%02u.%03u | ", t.wHour, t.wMinute, t.wSecond,
                                  t.wMilliseconds);
            g_at_line_start = 0;
        }
        if (text[i] == '\n') {
            buf[n++] = '\r';
            g_at_line_start = 1;
        }
        buf[n++] = text[i];
    }
    DWORD written = 0;
    WriteFile(g_live, buf, (DWORD)n, &written, NULL);
}

static int vout(int to_live, const char *fmt, va_list ap) {
    char text[4096];
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    if (n < 0) return n;
    size_t len = (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1;
    fwrite(text, 1, len, stdout);
    fflush(stdout);
    if (to_live) live_write(text, len);
    return n;
}

int out_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vout(1, fmt, ap);
    va_end(ap);
    return n;
}

int out_printf_console(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vout(0, fmt, ap);
    va_end(ap);
    return n;
}
