/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows backend of the mt792x HAL. See hal_win.h.
 *
 * DMA memory: AllocateUserPhysicalPages2 with the non-paged-large attribute returns locked
 * 2 MiB blocks with known physical frame numbers (technique measured in docs/FINDINGS.md F-005,
 * first described by the winawdl project). We ask for many, keep the lowest one that lies
 * entirely below 4 GiB, and free the rest immediately. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_win.h"
#include "../proxy/mtdrop_proxy_ioctl.h"

#ifndef MEM_EXTENDED_PARAMETER_NONPAGED_LARGE
#define MEM_EXTENDED_PARAMETER_NONPAGED_LARGE 0x00000008
#endif

#define PAGE_BYTES 4096u
#define PAGES_PER_BLOCK (HALWIN_DMA_BLOCK_BYTES / PAGE_BYTES)
#define FRAME_AT_4GIB (0x100000000ull / PAGE_BYTES)
#define BLOCKS_REQUESTED 64u

typedef BOOL(WINAPI *AllocatePages2Fn)(HANDLE, PULONG_PTR, PULONG_PTR, PMEM_EXTENDED_PARAMETER, ULONG);

struct halwin {
    struct mt_hal hal;
    HANDLE proxy;
    ULONG_PTR block_frame;      /* first 4 KiB frame of our block; 0 = none */
    ULONG_PTR frames[PAGES_PER_BLOCK];
    void *view;
    uint32_t low_blocks_seen;
    uint32_t io_errors;
    LARGE_INTEGER qpc_frequency;
};

static void set_error(char *error, size_t len, const char *text, DWORD code) {
    if (error && len) snprintf(error, len, "%s (Windows error %lu)", text, (unsigned long)code);
}

static BOOL enable_lock_memory_privilege(void) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return FALSE;
    TOKEN_PRIVILEGES tp;
    memset(&tp, 0, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = LookupPrivilegeValueW(NULL, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid);
    if (ok) {
        AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
        ok = GetLastError() == ERROR_SUCCESS; /* succeeds with ERROR_NOT_ALL_ASSIGNED when not held */
    }
    CloseHandle(token);
    return ok;
}

static BOOL batch(struct halwin *h, REGPROXY_OP *ops, DWORD count) {
    DWORD bytes = count * (DWORD)sizeof(*ops), got = 0;
    if (h->proxy == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(h->proxy, IOCTL_REGPROXY_BATCH, ops, bytes, ops, bytes, &got, NULL) || got != bytes) {
        h->io_errors++;
        return FALSE;
    }
    return TRUE;
}

static uint32_t hal_rr(void *ctx, uint32_t offset) {
    REGPROXY_OP op;
    memset(&op, 0, sizeof(op));
    op.offset = offset;
    op.width = 4;
    op.kind = REGPROXY_READ;
    /* All ones is what a dead bus reads; callers treat it as "not ready". */
    return batch((struct halwin *)ctx, &op, 1) ? op.value : 0xFFFFFFFFu;
}

static void hal_wr(void *ctx, uint32_t offset, uint32_t value) {
    REGPROXY_OP op;
    memset(&op, 0, sizeof(op));
    op.offset = offset;
    op.value = value;
    op.width = 4;
    op.kind = REGPROXY_WRITE;
    batch((struct halwin *)ctx, &op, 1);
}

static uint64_t hal_now_us(void *ctx) {
    struct halwin *h = (struct halwin *)ctx;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / h->qpc_frequency.QuadPart) * 1000000ull +
           (uint64_t)(now.QuadPart % h->qpc_frequency.QuadPart) * 1000000ull / (uint64_t)h->qpc_frequency.QuadPart;
}

static void hal_sleep_us(void *ctx, uint32_t us) {
    if (us >= 2000) {
        Sleep(us / 1000);
        return;
    }
    uint64_t end = hal_now_us(ctx) + us; /* short waits: spin, Sleep() is too coarse */
    while (hal_now_us(ctx) < end) YieldProcessor();
}

static void hal_log(void *ctx, enum mt_log_level level, const char *message) {
    static const char *const names[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
    (void)ctx;
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("%02u:%02u:%02u.%03u %s %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
           names[level <= MT_LOG_DEBUG ? level : MT_LOG_DEBUG], message);
    fflush(stdout);
}

struct halwin *halwin_create(char *error, size_t error_len) {
    struct halwin *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->proxy = INVALID_HANDLE_VALUE;
    QueryPerformanceFrequency(&h->qpc_frequency);

    if (!enable_lock_memory_privilege()) {
        set_error(error, error_len, "SeLockMemoryPrivilege is not held; run as SYSTEM", GetLastError());
        free(h);
        return NULL;
    }
    AllocatePages2Fn allocate = NULL;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) allocate = (AllocatePages2Fn)(void *)GetProcAddress(kernelbase, "AllocateUserPhysicalPages2");
    if (!allocate) {
        set_error(error, error_len, "AllocateUserPhysicalPages2 is not available", GetLastError());
        free(h);
        return NULL;
    }

    ULONG_PTR blocks[BLOCKS_REQUESTED];
    ULONG_PTR count = BLOCKS_REQUESTED;
    MEM_EXTENDED_PARAMETER large;
    memset(&large, 0, sizeof(large));
    large.Type = MemExtendedParameterAttributeFlags;
    large.ULong64 = MEM_EXTENDED_PARAMETER_NONPAGED_LARGE;
    if (!allocate(GetCurrentProcess(), &count, blocks, &large, 1) || count == 0) {
        set_error(error, error_len, "no 2 MiB locked blocks could be allocated", GetLastError());
        free(h);
        return NULL;
    }

    /* Keep the lowest block that ends below 4 GiB; free every other block one by one. */
    ULONG_PTR best = 0;
    for (ULONG_PTR i = 0; i < count; i++) {
        if (blocks[i] + PAGES_PER_BLOCK > FRAME_AT_4GIB) continue;
        h->low_blocks_seen++;
        if (!best || blocks[i] < best) best = blocks[i];
    }
    for (ULONG_PTR i = 0; i < count; i++) {
        if (blocks[i] == best) continue;
        ULONG_PTR one = 1;
        FreeUserPhysicalPages(GetCurrentProcess(), &one, &blocks[i]);
    }
    if (!best) {
        set_error(error, error_len, "no free 2 MiB block below 4 GiB right now (try again, or after a reboot)", 0);
        free(h);
        return NULL;
    }
    h->block_frame = best;

    /* A block maps only as 512 individual frames into a plain MEM_PHYSICAL view (F-005). */
    for (ULONG_PTR i = 0; i < PAGES_PER_BLOCK; i++) h->frames[i] = best + i;
    h->view = VirtualAlloc(NULL, HALWIN_DMA_BLOCK_BYTES, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    if (!h->view || !MapUserPhysicalPages(h->view, PAGES_PER_BLOCK, h->frames)) {
        set_error(error, error_len, "could not map the DMA block", GetLastError());
        halwin_destroy(h);
        return NULL;
    }
    memset(h->view, 0, HALWIN_DMA_BLOCK_BYTES);

    h->hal.ctx = h;
    h->hal.rr = hal_rr;
    h->hal.wr = hal_wr;
    h->hal.sleep_us = hal_sleep_us;
    h->hal.now_us = hal_now_us;
    h->hal.log = hal_log;
    h->hal.dma_virt = (uint8_t *)h->view;
    h->hal.dma_bus = (uint32_t)((uint64_t)best * PAGE_BYTES);
    h->hal.dma_len = HALWIN_DMA_BLOCK_BYTES;
    return h;
}

int halwin_open_proxy(struct halwin *h, char *error, size_t error_len) {
    GUID guid = GUID_DEVINTERFACE_MTDROP_REGPROXY;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, &guid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length <= 1) {
        set_error(error, error_len, "the mtdrop proxy driver is not bound to the card", 0);
        return -1;
    }
    WCHAR *list = calloc(length, sizeof(WCHAR));
    if (!list) return -1;
    if (CM_Get_Device_Interface_ListW(&guid, NULL, list, length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || !list[0]) {
        free(list);
        set_error(error, error_len, "cannot list the proxy device interface", 0);
        return -1;
    }
    h->proxy = CreateFileW(list, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    free(list);
    if (h->proxy == INVALID_HANDLE_VALUE) {
        set_error(error, error_len, "cannot open the proxy device", GetLastError());
        return -1;
    }
    REGPROXY_INFO info;
    DWORD got = 0;
    if (!DeviceIoControl(h->proxy, IOCTL_REGPROXY_GET_INFO, NULL, 0, &info, sizeof(info), &got, NULL) ||
        got != sizeof(info) || info.bar_len < 0x100000u || info.version < 2u) {
        set_error(error, error_len, "proxy is too old or did not report a 1 MiB register window", GetLastError());
        CloseHandle(h->proxy);
        h->proxy = INVALID_HANDLE_VALUE;
        return -1;
    }
    return 0;
}

int halwin_set_close_ops(struct halwin *h, const struct halwin_close_op *ops, uint32_t count) {
    REGPROXY_OP raw[REGPROXY_MAX_CLOSE_OPS];
    if (count > REGPROXY_MAX_CLOSE_OPS || h->proxy == INVALID_HANDLE_VALUE) return -1;
    memset(raw, 0, sizeof(raw));
    for (uint32_t i = 0; i < count; i++) {
        if (ops[i].kind < REGPROXY_WRITE || ops[i].kind > REGPROXY_DELAY) return -1;
        raw[i].offset = ops[i].offset;
        raw[i].value = ops[i].value;
        raw[i].width = 4;
        raw[i].kind = ops[i].kind;
    }
    DWORD got = 0;
    return DeviceIoControl(h->proxy, IOCTL_REGPROXY_SET_CLOSE_OPS, raw, count * (DWORD)sizeof(raw[0]), NULL, 0, &got, NULL) ? 0 : -1;
}

struct mt_hal *halwin_hal(struct halwin *h) { return &h->hal; }
uint64_t halwin_dma_phys(const struct halwin *h) { return (uint64_t)h->block_frame * PAGE_BYTES; }
uint32_t halwin_low_blocks_seen(const struct halwin *h) { return h->low_blocks_seen; }
uint32_t halwin_io_errors(const struct halwin *h) { return h->io_errors; }

void halwin_destroy(struct halwin *h) {
    if (!h) return;
    /* Order matters: closing the handle makes the proxy stop the chip's DMA (close ops)
     * BEFORE the memory the chip may still be using is returned to Windows. */
    if (h->proxy != INVALID_HANDLE_VALUE) CloseHandle(h->proxy);
    if (h->view) {
        MapUserPhysicalPages(h->view, PAGES_PER_BLOCK, NULL);
        VirtualFree(h->view, 0, MEM_RELEASE);
    }
    if (h->block_frame) {
        ULONG_PTR one = 1;
        FreeUserPhysicalPages(GetCurrentProcess(), &one, &h->block_frame);
    }
    free(h);
}
