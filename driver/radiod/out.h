/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radiod console output with an optional write-through mirror ("live log").
 *
 * Why (docs/FINDINGS.md F-011): the session script used to collect radiod's output and log it only
 * after the process exited, so when the whole system hung mid-run nothing of that run survived.
 * Every line written here reaches the live log file on disk before the call returns
 * (FILE_FLAG_WRITE_THROUGH), so the last line in the file is the last thing radiod did. */
#ifndef MTDROP_RADIOD_OUT_H
#define MTDROP_RADIOD_OUT_H

/* Opens (appends to) the live log. Returns 0, or -1 if the file cannot be opened. */
int out_open_live(const char *path);
void out_close_live(void);

/* printf to stdout (flushed) and, if open, to the live log. */
int out_printf(const char *fmt, ...);
/* Same text to stdout only (DEBUG messages: too many for a write-through file). */
int out_printf_console(const char *fmt, ...);

#endif
